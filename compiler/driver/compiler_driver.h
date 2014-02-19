/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ART_COMPILER_DRIVER_COMPILER_DRIVER_H_
#define ART_COMPILER_DRIVER_COMPILER_DRIVER_H_

#include <set>
#include <string>
#include <vector>

#include "base/mutex.h"
#include "base/timing_logger.h"
#include "class_reference.h"
#include "compiled_class.h"
#include "compiled_method.h"
#include "compiler_backend.h"
#include "dex_file.h"
#include "instruction_set.h"
#include "invoke_type.h"
#include "method_reference.h"
#include "os.h"
#include "runtime.h"
#include "safe_map.h"
#include "thread_pool.h"
#include "utils/arena_allocator.h"
#include "utils/dedupe_set.h"

namespace art {

namespace verifier {
class MethodVerifier;
}  // namespace verifier

class AOTCompilationStats;
class DexCompilationUnit;
class DexFileToMethodInlinerMap;
class InlineIGetIPutData;
class OatWriter;
class ParallelCompilationManager;
class TimingLogger;
class VerificationResults;
class VerifiedMethod;

enum EntryPointCallingConvention {
  // ABI of invocations to a method's interpreter entry point.
  kInterpreterAbi,
  // ABI of calls to a method's native code, only used for native methods.
  kJniAbi,
  // ABI of calls to a method's portable code entry point.
  kPortableAbi,
  // ABI of calls to a method's quick code entry point.
  kQuickAbi
};

enum DexToDexCompilationLevel {
  kDontDexToDexCompile,   // Only meaning wrt image time interpretation.
  kRequired,              // Dex-to-dex compilation required for correctness.
  kOptimize               // Perform required transformation and peep-hole optimizations.
};

// Thread-local storage compiler worker threads
class CompilerTls {
  public:
    CompilerTls() : llvm_info_(NULL) {}
    ~CompilerTls() {}

    void* GetLLVMInfo() { return llvm_info_; }

    void SetLLVMInfo(void* llvm_info) { llvm_info_ = llvm_info; }

  private:
    void* llvm_info_;
};

class CompilerDriver {
 public:
  typedef std::set<std::string> DescriptorSet;

  // Create a compiler targeting the requested "instruction_set".
  // "image" should be true if image specific optimizations should be
  // enabled.  "image_classes" lets the compiler know what classes it
  // can assume will be in the image, with NULL implying all available
  // classes.
  explicit CompilerDriver(VerificationResults* verification_results,
                          DexFileToMethodInlinerMap* method_inliner_map,
                          CompilerBackend::Kind compiler_backend_kind,
                          InstructionSet instruction_set,
                          InstructionSetFeatures instruction_set_features,
                          bool image, DescriptorSet* image_classes,
                          size_t thread_count, bool dump_stats, bool dump_passes,
                          CumulativeLogger* timer);

  ~CompilerDriver();

  void CompileAll(jobject class_loader, const std::vector<const DexFile*>& dex_files,
                  TimingLogger& timings)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  // Compile a single Method.
  void CompileOne(mirror::ArtMethod* method, TimingLogger& timings)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  VerificationResults* GetVerificationResults() const {
    return verification_results_;
  }

  DexFileToMethodInlinerMap* GetMethodInlinerMap() const {
    return method_inliner_map_;
  }

  const InstructionSet& GetInstructionSet() const {
    return instruction_set_;
  }

  const InstructionSetFeatures& GetInstructionSetFeatures() const {
    return instruction_set_features_;
  }

  CompilerBackend* GetCompilerBackend() const {
    return compiler_backend_.get();
  }

  // Are we compiling and creating an image file?
  bool IsImage() const {
    return image_;
  }

  DescriptorSet* GetImageClasses() const {
    return image_classes_.get();
  }

  CompilerTls* GetTls();

  // Generate the trampolines that are invoked by unresolved direct methods.
  const std::vector<uint8_t>* CreateInterpreterToInterpreterBridge() const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  const std::vector<uint8_t>* CreateInterpreterToCompiledCodeBridge() const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  const std::vector<uint8_t>* CreateJniDlsymLookup() const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  const std::vector<uint8_t>* CreatePortableImtConflictTrampoline() const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  const std::vector<uint8_t>* CreatePortableResolutionTrampoline() const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  const std::vector<uint8_t>* CreatePortableToInterpreterBridge() const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  const std::vector<uint8_t>* CreateQuickImtConflictTrampoline() const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  const std::vector<uint8_t>* CreateQuickResolutionTrampoline() const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  const std::vector<uint8_t>* CreateQuickToInterpreterBridge() const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  CompiledClass* GetCompiledClass(ClassReference ref) const
      LOCKS_EXCLUDED(compiled_classes_lock_);

  CompiledMethod* GetCompiledMethod(MethodReference ref) const
      LOCKS_EXCLUDED(compiled_methods_lock_);

  void AddRequiresConstructorBarrier(Thread* self, const DexFile* dex_file,
                                     uint16_t class_def_index);
  bool RequiresConstructorBarrier(Thread* self, const DexFile* dex_file, uint16_t class_def_index);

  // Callbacks from compiler to see what runtime checks must be generated.

  bool CanAssumeTypeIsPresentInDexCache(const DexFile& dex_file, uint32_t type_idx);

  bool CanAssumeStringIsPresentInDexCache(const DexFile& dex_file, uint32_t string_idx)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  // Are runtime access checks necessary in the compiled code?
  bool CanAccessTypeWithoutChecks(uint32_t referrer_idx, const DexFile& dex_file,
                                  uint32_t type_idx, bool* type_known_final = NULL,
                                  bool* type_known_abstract = NULL,
                                  bool* equals_referrers_class = NULL)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  // Are runtime access and instantiable checks necessary in the code?
  bool CanAccessInstantiableTypeWithoutChecks(uint32_t referrer_idx, const DexFile& dex_file,
                                              uint32_t type_idx)
     LOCKS_EXCLUDED(Locks::mutator_lock_);

  bool CanEmbedTypeInCode(const DexFile& dex_file, uint32_t type_idx,
                          bool* is_type_initialized, bool* use_direct_type_ptr,
                          uintptr_t* direct_type_ptr);

  // Can we fast path instance field access in a verified accessor?
  // If yes, computes field's offset and volatility and whether the method is static or not.
  static bool ComputeSpecialAccessorInfo(uint32_t field_idx, bool is_put,
                                         verifier::MethodVerifier* verifier,
                                         InlineIGetIPutData* result)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Can we fast path instance field access? Computes field's offset and volatility.
  bool ComputeInstanceFieldInfo(uint32_t field_idx, const DexCompilationUnit* mUnit, bool is_put,
                                int* field_offset, bool* is_volatile)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  // Can we fastpath static field access? Computes field's offset, volatility and whether the
  // field is within the referrer (which can avoid checking class initialization).
  bool ComputeStaticFieldInfo(uint32_t field_idx, const DexCompilationUnit* mUnit, bool is_put,
                              int* field_offset, int* storage_index,
                              bool* is_referrers_class, bool* is_volatile, bool* is_initialized)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  // Can we fastpath a interface, super class or virtual method call? Computes method's vtable
  // index.
  bool ComputeInvokeInfo(const DexCompilationUnit* mUnit, const uint32_t dex_pc,
                         bool update_stats, bool enable_devirtualization,
                         InvokeType* type, MethodReference* target_method, int* vtable_idx,
                         uintptr_t* direct_code, uintptr_t* direct_method)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  const VerifiedMethod* GetVerifiedMethod(const DexFile* dex_file, uint32_t method_idx) const;
  bool IsSafeCast(const DexCompilationUnit* mUnit, uint32_t dex_pc);

  // Record patch information for later fix up.
  void AddCodePatch(const DexFile* dex_file,
                    uint16_t referrer_class_def_idx,
                    uint32_t referrer_method_idx,
                    InvokeType referrer_invoke_type,
                    uint32_t target_method_idx,
                    InvokeType target_invoke_type,
                    size_t literal_offset)
      LOCKS_EXCLUDED(compiled_methods_lock_);
  void AddRelativeCodePatch(const DexFile* dex_file,
                            uint16_t referrer_class_def_idx,
                            uint32_t referrer_method_idx,
                            InvokeType referrer_invoke_type,
                            uint32_t target_method_idx,
                            InvokeType target_invoke_type,
                            size_t literal_offset,
                            int32_t pc_relative_offset)
      LOCKS_EXCLUDED(compiled_methods_lock_);
  void AddMethodPatch(const DexFile* dex_file,
                      uint16_t referrer_class_def_idx,
                      uint32_t referrer_method_idx,
                      InvokeType referrer_invoke_type,
                      uint32_t target_method_idx,
                      InvokeType target_invoke_type,
                      size_t literal_offset)
      LOCKS_EXCLUDED(compiled_methods_lock_);
  void AddClassPatch(const DexFile* dex_file,
                     uint16_t referrer_class_def_idx,
                     uint32_t referrer_method_idx,
                     uint32_t target_method_idx,
                     size_t literal_offset)
      LOCKS_EXCLUDED(compiled_methods_lock_);

  void SetBitcodeFileName(std::string const& filename);

  bool GetSupportBootImageFixup() const {
    return support_boot_image_fixup_;
  }

  void SetSupportBootImageFixup(bool support_boot_image_fixup) {
    support_boot_image_fixup_ = support_boot_image_fixup;
  }

  ArenaPool& GetArenaPool() {
    return arena_pool_;
  }

  bool WriteElf(const std::string& android_root,
                bool is_host,
                const std::vector<const DexFile*>& dex_files,
                OatWriter& oat_writer,
                File* file);

  // TODO: move to a common home for llvm helpers once quick/portable are merged
  static void InstructionSetToLLVMTarget(InstructionSet instruction_set,
                                         std::string& target_triple,
                                         std::string& target_cpu,
                                         std::string& target_attr);

  void SetCompilerContext(void* compiler_context) {
    compiler_context_ = compiler_context;
  }

  void* GetCompilerContext() const {
    return compiler_context_;
  }

  size_t GetThreadCount() const {
    return thread_count_;
  }

  class CallPatchInformation;
  class TypePatchInformation;

  bool GetDumpPasses() const {
    return dump_passes_;
  }

  CumulativeLogger& GetTimingsLogger() const {
    return *timings_logger_;
  }

  class PatchInformation {
   public:
    const DexFile& GetDexFile() const {
      return *dex_file_;
    }
    uint16_t GetReferrerClassDefIdx() const {
      return referrer_class_def_idx_;
    }
    uint32_t GetReferrerMethodIdx() const {
      return referrer_method_idx_;
    }
    size_t GetLiteralOffset() const {
      return literal_offset_;
    }

    virtual bool IsCall() const {
      return false;
    }
    virtual bool IsType() const {
      return false;
    }
    virtual const CallPatchInformation* AsCall() const {
      LOG(FATAL) << "Unreachable";
      return nullptr;
    }
    virtual const TypePatchInformation* AsType() const {
      LOG(FATAL) << "Unreachable";
      return nullptr;
    }

   protected:
    PatchInformation(const DexFile* dex_file,
                     uint16_t referrer_class_def_idx,
                     uint32_t referrer_method_idx,
                     size_t literal_offset)
      : dex_file_(dex_file),
        referrer_class_def_idx_(referrer_class_def_idx),
        referrer_method_idx_(referrer_method_idx),
        literal_offset_(literal_offset) {
      CHECK(dex_file_ != NULL);
    }
    virtual ~PatchInformation() {}

    const DexFile* const dex_file_;
    const uint16_t referrer_class_def_idx_;
    const uint32_t referrer_method_idx_;
    const size_t literal_offset_;

    friend class CompilerDriver;
  };

  class CallPatchInformation : public PatchInformation {
   public:
    InvokeType GetReferrerInvokeType() const {
      return referrer_invoke_type_;
    }
    uint32_t GetTargetMethodIdx() const {
      return target_method_idx_;
    }
    InvokeType GetTargetInvokeType() const {
      return target_invoke_type_;
    }

    const CallPatchInformation* AsCall() const {
      return this;
    }
    bool IsCall() const {
      return true;
    }
    virtual bool IsRelative() const {
      return false;
    }
    virtual int RelativeOffset() const {
      return 0;
    }

   protected:
    CallPatchInformation(const DexFile* dex_file,
                         uint16_t referrer_class_def_idx,
                         uint32_t referrer_method_idx,
                         InvokeType referrer_invoke_type,
                         uint32_t target_method_idx,
                         InvokeType target_invoke_type,
                         size_t literal_offset)
        : PatchInformation(dex_file, referrer_class_def_idx,
                           referrer_method_idx, literal_offset),
          referrer_invoke_type_(referrer_invoke_type),
          target_method_idx_(target_method_idx),
          target_invoke_type_(target_invoke_type) {
    }

   private:
    const InvokeType referrer_invoke_type_;
    const uint32_t target_method_idx_;
    const InvokeType target_invoke_type_;

    friend class CompilerDriver;
    DISALLOW_COPY_AND_ASSIGN(CallPatchInformation);
  };

  class RelativeCallPatchInformation : public CallPatchInformation {
   public:
    bool IsRelative() const {
      return true;
    }
    int RelativeOffset() const {
      return offset_;
    }

   private:
    RelativeCallPatchInformation(const DexFile* dex_file,
                                 uint16_t referrer_class_def_idx,
                                 uint32_t referrer_method_idx,
                                 InvokeType referrer_invoke_type,
                                 uint32_t target_method_idx,
                                 InvokeType target_invoke_type,
                                 size_t literal_offset,
                                 int32_t pc_relative_offset)
        : CallPatchInformation(dex_file, referrer_class_def_idx,
                           referrer_method_idx, referrer_invoke_type,
                           target_method_idx, target_invoke_type, literal_offset),
          offset_(pc_relative_offset) {
    }

    const int offset_;

    friend class CompilerDriver;
    DISALLOW_COPY_AND_ASSIGN(RelativeCallPatchInformation);
  };

  class TypePatchInformation : public PatchInformation {
   public:
    uint32_t GetTargetTypeIdx() const {
      return target_type_idx_;
    }

    bool IsType() const {
      return true;
    }
    const TypePatchInformation* AsType() const {
      return this;
    }

   private:
    TypePatchInformation(const DexFile* dex_file,
                         uint16_t referrer_class_def_idx,
                         uint32_t referrer_method_idx,
                         uint32_t target_type_idx,
                         size_t literal_offset)
        : PatchInformation(dex_file, referrer_class_def_idx,
                           referrer_method_idx, literal_offset),
          target_type_idx_(target_type_idx) {
    }

    const uint32_t target_type_idx_;

    friend class CompilerDriver;
    DISALLOW_COPY_AND_ASSIGN(TypePatchInformation);
  };

  const std::vector<const CallPatchInformation*>& GetCodeToPatch() const {
    return code_to_patch_;
  }
  const std::vector<const CallPatchInformation*>& GetMethodsToPatch() const {
    return methods_to_patch_;
  }
  const std::vector<const TypePatchInformation*>& GetClassesToPatch() const {
    return classes_to_patch_;
  }

  // Checks if class specified by type_idx is one of the image_classes_
  bool IsImageClass(const char* descriptor) const;

  void RecordClassStatus(ClassReference ref, mirror::Class::Status status)
      LOCKS_EXCLUDED(compiled_classes_lock_);

  std::vector<uint8_t>* DeduplicateCode(const std::vector<uint8_t>& code);
  std::vector<uint8_t>* DeduplicateMappingTable(const std::vector<uint8_t>& code);
  std::vector<uint8_t>* DeduplicateVMapTable(const std::vector<uint8_t>& code);
  std::vector<uint8_t>* DeduplicateGCMap(const std::vector<uint8_t>& code);

 private:
  // Compute constant code and method pointers when possible
  void GetCodeAndMethodForDirectCall(InvokeType* type, InvokeType sharp_type,
                                     bool no_guarantee_of_dex_cache_entry,
                                     mirror::Class* referrer_class,
                                     mirror::ArtMethod* method,
                                     bool update_stats,
                                     MethodReference* target_method,
                                     uintptr_t* direct_code, uintptr_t* direct_method)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void PreCompile(jobject class_loader, const std::vector<const DexFile*>& dex_files,
                  ThreadPool& thread_pool, TimingLogger& timings)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  void LoadImageClasses(TimingLogger& timings);

  // Attempt to resolve all type, methods, fields, and strings
  // referenced from code in the dex file following PathClassLoader
  // ordering semantics.
  void Resolve(jobject class_loader, const std::vector<const DexFile*>& dex_files,
               ThreadPool& thread_pool, TimingLogger& timings)
      LOCKS_EXCLUDED(Locks::mutator_lock_);
  void ResolveDexFile(jobject class_loader, const DexFile& dex_file,
                      ThreadPool& thread_pool, TimingLogger& timings)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  void Verify(jobject class_loader, const std::vector<const DexFile*>& dex_files,
              ThreadPool& thread_pool, TimingLogger& timings);
  void VerifyDexFile(jobject class_loader, const DexFile& dex_file,
                     ThreadPool& thread_pool, TimingLogger& timings)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  void InitializeClasses(jobject class_loader, const std::vector<const DexFile*>& dex_files,
                         ThreadPool& thread_pool, TimingLogger& timings)
      LOCKS_EXCLUDED(Locks::mutator_lock_);
  void InitializeClasses(jobject class_loader, const DexFile& dex_file,
                         ThreadPool& thread_pool, TimingLogger& timings)
      LOCKS_EXCLUDED(Locks::mutator_lock_, compiled_classes_lock_);

  void UpdateImageClasses(TimingLogger& timings)
      LOCKS_EXCLUDED(Locks::mutator_lock_);
  static void FindClinitImageClassesCallback(mirror::Object* object, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void Compile(jobject class_loader, const std::vector<const DexFile*>& dex_files,
               ThreadPool& thread_pool, TimingLogger& timings);
  void CompileDexFile(jobject class_loader, const DexFile& dex_file,
                      ThreadPool& thread_pool, TimingLogger& timings)
      LOCKS_EXCLUDED(Locks::mutator_lock_);
  void CompileMethod(const DexFile::CodeItem* code_item, uint32_t access_flags,
                     InvokeType invoke_type, uint16_t class_def_idx, uint32_t method_idx,
                     jobject class_loader, const DexFile& dex_file,
                     DexToDexCompilationLevel dex_to_dex_compilation_level)
      LOCKS_EXCLUDED(compiled_methods_lock_);

  static void CompileClass(const ParallelCompilationManager* context, size_t class_def_index)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  std::vector<const CallPatchInformation*> code_to_patch_;
  std::vector<const CallPatchInformation*> methods_to_patch_;
  std::vector<const TypePatchInformation*> classes_to_patch_;

  VerificationResults* verification_results_;
  DexFileToMethodInlinerMap* method_inliner_map_;

  UniquePtr<CompilerBackend> compiler_backend_;

  const InstructionSet instruction_set_;
  const InstructionSetFeatures instruction_set_features_;

  // All class references that require
  mutable ReaderWriterMutex freezing_constructor_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  std::set<ClassReference> freezing_constructor_classes_ GUARDED_BY(freezing_constructor_lock_);

  typedef SafeMap<const ClassReference, CompiledClass*> ClassTable;
  // All class references that this compiler has compiled.
  mutable Mutex compiled_classes_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  ClassTable compiled_classes_ GUARDED_BY(compiled_classes_lock_);

  typedef SafeMap<const MethodReference, CompiledMethod*, MethodReferenceComparator> MethodTable;
  // All method references that this compiler has compiled.
  mutable Mutex compiled_methods_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  MethodTable compiled_methods_ GUARDED_BY(compiled_methods_lock_);

  const bool image_;

  // If image_ is true, specifies the classes that will be included in
  // the image. Note if image_classes_ is NULL, all classes are
  // included in the image.
  UniquePtr<DescriptorSet> image_classes_;

  size_t thread_count_;
  uint64_t start_ns_;

  UniquePtr<AOTCompilationStats> stats_;

  bool dump_stats_;
  const bool dump_passes_;

  CumulativeLogger* const timings_logger_;

  typedef void (*CompilerCallbackFn)(CompilerDriver& driver);
  typedef MutexLock* (*CompilerMutexLockFn)(CompilerDriver& driver);

  void* compiler_library_;

  typedef void (*DexToDexCompilerFn)(CompilerDriver& driver,
                                     const DexFile::CodeItem* code_item,
                                     uint32_t access_flags, InvokeType invoke_type,
                                     uint32_t class_dex_idx, uint32_t method_idx,
                                     jobject class_loader, const DexFile& dex_file,
                                     DexToDexCompilationLevel dex_to_dex_compilation_level);
  DexToDexCompilerFn dex_to_dex_compiler_;

  void* compiler_context_;

  pthread_key_t tls_key_;

  // Arena pool used by the compiler.
  ArenaPool arena_pool_;

  typedef void (*CompilerEnableAutoElfLoadingFn)(CompilerDriver& driver);
  CompilerEnableAutoElfLoadingFn compiler_enable_auto_elf_loading_;

  typedef const void* (*CompilerGetMethodCodeAddrFn)
      (const CompilerDriver& driver, const CompiledMethod* cm, const mirror::ArtMethod* method);
  CompilerGetMethodCodeAddrFn compiler_get_method_code_addr_;

  bool support_boot_image_fixup_;

  // DeDuplication data structures, these own the corresponding byte arrays.
  class DedupeHashFunc {
   public:
    size_t operator()(const std::vector<uint8_t>& array) const {
      // For small arrays compute a hash using every byte.
      static const size_t kSmallArrayThreshold = 16;
      size_t hash = 0x811c9dc5;
      if (array.size() <= kSmallArrayThreshold) {
        for (uint8_t b : array) {
          hash = (hash * 16777619) ^ b;
        }
      } else {
        // For larger arrays use the 2 bytes at 6 bytes (the location of a push registers
        // instruction field for quick generated code on ARM) and then select a number of other
        // values at random.
        static const size_t kRandomHashCount = 16;
        for (size_t i = 0; i < 2; ++i) {
          uint8_t b = array[i + 6];
          hash = (hash * 16777619) ^ b;
        }
        for (size_t i = 2; i < kRandomHashCount; ++i) {
          size_t r = i * 1103515245 + 12345;
          uint8_t b = array[r % array.size()];
          hash = (hash * 16777619) ^ b;
        }
      }
      hash += hash << 13;
      hash ^= hash >> 7;
      hash += hash << 3;
      hash ^= hash >> 17;
      hash += hash << 5;
      return hash;
    }
  };
  DedupeSet<std::vector<uint8_t>, size_t, DedupeHashFunc, 4> dedupe_code_;
  DedupeSet<std::vector<uint8_t>, size_t, DedupeHashFunc, 4> dedupe_mapping_table_;
  DedupeSet<std::vector<uint8_t>, size_t, DedupeHashFunc, 4> dedupe_vmap_table_;
  DedupeSet<std::vector<uint8_t>, size_t, DedupeHashFunc, 4> dedupe_gc_map_;

  DISALLOW_COPY_AND_ASSIGN(CompilerDriver);
};

}  // namespace art

#endif  // ART_COMPILER_DRIVER_COMPILER_DRIVER_H_
