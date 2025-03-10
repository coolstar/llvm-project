//===--- BackendUtil.cpp - LLVM Backend Utilities -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/CodeGen/BackendUtil.h"
#include "clang/Basic/CodeGenOptions.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/TargetOptions.h"
#include "clang/Frontend/FrontendDiagnostic.h"
#include "clang/Frontend/Utils.h"
#include "clang/Lex/HeaderSearchOptions.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Analysis/StackSafetyAnalysis.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/Bitcode/BitcodeWriterPass.h"
#include "llvm/CodeGen/RegAllocRegistry.h"
#include "llvm/CodeGen/SchedulerRegistry.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ModuleSummaryIndex.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/LTO/LTOBackend.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/SubtargetFeature.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Passes/StandardInstrumentations.h"
#include "llvm/Support/BuryPointer.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/TimeProfiler.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/Transforms/Coroutines.h"
#include "llvm/Transforms/Coroutines/CoroCleanup.h"
#include "llvm/Transforms/Coroutines/CoroEarly.h"
#include "llvm/Transforms/Coroutines/CoroElide.h"
#include "llvm/Transforms/Coroutines/CoroSplit.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/IPO/AlwaysInliner.h"
#include "llvm/Transforms/IPO/LowerTypeTests.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/IPO/ThinLTOBitcodeWriter.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Instrumentation.h"
#include "llvm/Transforms/Instrumentation/AddressSanitizer.h"
#include "llvm/Transforms/Instrumentation/BoundsChecking.h"
#include "llvm/Transforms/Instrumentation/GCOVProfiler.h"
#include "llvm/Transforms/Instrumentation/HWAddressSanitizer.h"
#include "llvm/Transforms/Instrumentation/InstrProfiling.h"
#include "llvm/Transforms/Instrumentation/MemorySanitizer.h"
#include "llvm/Transforms/Instrumentation/SanitizerCoverage.h"
#include "llvm/Transforms/Instrumentation/ThreadSanitizer.h"
#include "llvm/Transforms/ObjCARC.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Transforms/Utils.h"
#include "llvm/Transforms/Utils/CanonicalizeAliases.h"
#include "llvm/Transforms/Utils/EntryExitInstrumenter.h"
#include "llvm/Transforms/Utils/NameAnonGlobals.h"
#include "llvm/Transforms/Utils/SymbolRewriter.h"
#include "llvm/Transforms/Utils/UniqueInternalLinkageNames.h"
#include <memory>
using namespace clang;
using namespace llvm;

#define HANDLE_EXTENSION(Ext)                                                  \
  llvm::PassPluginLibraryInfo get##Ext##PluginInfo();
#include "llvm/Support/Extension.def"

namespace {

// Default filename used for profile generation.
static constexpr StringLiteral DefaultProfileGenName = "default_%m.profraw";

class EmitAssemblyHelper {
  DiagnosticsEngine &Diags;
  const HeaderSearchOptions &HSOpts;
  const CodeGenOptions &CodeGenOpts;
  const clang::TargetOptions &TargetOpts;
  const LangOptions &LangOpts;
  Module *TheModule;

  Timer CodeGenerationTime;

  std::unique_ptr<raw_pwrite_stream> OS;

  TargetIRAnalysis getTargetIRAnalysis() const {
    if (TM)
      return TM->getTargetIRAnalysis();

    return TargetIRAnalysis();
  }

  void CreatePasses(legacy::PassManager &MPM, legacy::FunctionPassManager &FPM);

  /// Generates the TargetMachine.
  /// Leaves TM unchanged if it is unable to create the target machine.
  /// Some of our clang tests specify triples which are not built
  /// into clang. This is okay because these tests check the generated
  /// IR, and they require DataLayout which depends on the triple.
  /// In this case, we allow this method to fail and not report an error.
  /// When MustCreateTM is used, we print an error if we are unable to load
  /// the requested target.
  void CreateTargetMachine(bool MustCreateTM);

  /// Add passes necessary to emit assembly or LLVM IR.
  ///
  /// \return True on success.
  bool AddEmitPasses(legacy::PassManager &CodeGenPasses, BackendAction Action,
                     raw_pwrite_stream &OS, raw_pwrite_stream *DwoOS);

  std::unique_ptr<llvm::ToolOutputFile> openOutputFile(StringRef Path) {
    std::error_code EC;
    auto F = std::make_unique<llvm::ToolOutputFile>(Path, EC,
                                                     llvm::sys::fs::OF_None);
    if (EC) {
      Diags.Report(diag::err_fe_unable_to_open_output) << Path << EC.message();
      F.reset();
    }
    return F;
  }

public:
  EmitAssemblyHelper(DiagnosticsEngine &_Diags,
                     const HeaderSearchOptions &HeaderSearchOpts,
                     const CodeGenOptions &CGOpts,
                     const clang::TargetOptions &TOpts,
                     const LangOptions &LOpts, Module *M)
      : Diags(_Diags), HSOpts(HeaderSearchOpts), CodeGenOpts(CGOpts),
        TargetOpts(TOpts), LangOpts(LOpts), TheModule(M),
        CodeGenerationTime("codegen", "Code Generation Time") {}

  ~EmitAssemblyHelper() {
    if (CodeGenOpts.DisableFree)
      BuryPointer(std::move(TM));
  }

  std::unique_ptr<TargetMachine> TM;

  void EmitAssembly(BackendAction Action,
                    std::unique_ptr<raw_pwrite_stream> OS);

  void EmitAssemblyWithNewPassManager(BackendAction Action,
                                      std::unique_ptr<raw_pwrite_stream> OS);
};

// We need this wrapper to access LangOpts and CGOpts from extension functions
// that we add to the PassManagerBuilder.
class PassManagerBuilderWrapper : public PassManagerBuilder {
public:
  PassManagerBuilderWrapper(const Triple &TargetTriple,
                            const CodeGenOptions &CGOpts,
                            const LangOptions &LangOpts)
      : PassManagerBuilder(), TargetTriple(TargetTriple), CGOpts(CGOpts),
        LangOpts(LangOpts) {}
  const Triple &getTargetTriple() const { return TargetTriple; }
  const CodeGenOptions &getCGOpts() const { return CGOpts; }
  const LangOptions &getLangOpts() const { return LangOpts; }

private:
  const Triple &TargetTriple;
  const CodeGenOptions &CGOpts;
  const LangOptions &LangOpts;
};
}

static void addObjCARCAPElimPass(const PassManagerBuilder &Builder, PassManagerBase &PM) {
  if (Builder.OptLevel > 0)
    PM.add(createObjCARCAPElimPass());
}

static void addObjCARCExpandPass(const PassManagerBuilder &Builder, PassManagerBase &PM) {
  if (Builder.OptLevel > 0)
    PM.add(createObjCARCExpandPass());
}

static void addObjCARCOptPass(const PassManagerBuilder &Builder, PassManagerBase &PM) {
  if (Builder.OptLevel > 0)
    PM.add(createObjCARCOptPass());
}

static void addAddDiscriminatorsPass(const PassManagerBuilder &Builder,
                                     legacy::PassManagerBase &PM) {
  PM.add(createAddDiscriminatorsPass());
}

static void addBoundsCheckingPass(const PassManagerBuilder &Builder,
                                  legacy::PassManagerBase &PM) {
  PM.add(createBoundsCheckingLegacyPass());
}

static SanitizerCoverageOptions
getSancovOptsFromCGOpts(const CodeGenOptions &CGOpts) {
  SanitizerCoverageOptions Opts;
  Opts.CoverageType =
      static_cast<SanitizerCoverageOptions::Type>(CGOpts.SanitizeCoverageType);
  Opts.IndirectCalls = CGOpts.SanitizeCoverageIndirectCalls;
  Opts.TraceBB = CGOpts.SanitizeCoverageTraceBB;
  Opts.TraceCmp = CGOpts.SanitizeCoverageTraceCmp;
  Opts.TraceDiv = CGOpts.SanitizeCoverageTraceDiv;
  Opts.TraceGep = CGOpts.SanitizeCoverageTraceGep;
  Opts.Use8bitCounters = CGOpts.SanitizeCoverage8bitCounters;
  Opts.TracePC = CGOpts.SanitizeCoverageTracePC;
  Opts.TracePCGuard = CGOpts.SanitizeCoverageTracePCGuard;
  Opts.NoPrune = CGOpts.SanitizeCoverageNoPrune;
  Opts.Inline8bitCounters = CGOpts.SanitizeCoverageInline8bitCounters;
  Opts.InlineBoolFlag = CGOpts.SanitizeCoverageInlineBoolFlag;
  Opts.PCTable = CGOpts.SanitizeCoveragePCTable;
  Opts.StackDepth = CGOpts.SanitizeCoverageStackDepth;
  return Opts;
}

static void addSanitizerCoveragePass(const PassManagerBuilder &Builder,
                                     legacy::PassManagerBase &PM) {
  const PassManagerBuilderWrapper &BuilderWrapper =
      static_cast<const PassManagerBuilderWrapper &>(Builder);
  const CodeGenOptions &CGOpts = BuilderWrapper.getCGOpts();
  auto Opts = getSancovOptsFromCGOpts(CGOpts);
  PM.add(createModuleSanitizerCoverageLegacyPassPass(
      Opts, CGOpts.SanitizeCoverageWhitelistFiles,
      CGOpts.SanitizeCoverageBlacklistFiles));
}

// Check if ASan should use GC-friendly instrumentation for globals.
// First of all, there is no point if -fdata-sections is off (expect for MachO,
// where this is not a factor). Also, on ELF this feature requires an assembler
// extension that only works with -integrated-as at the moment.
static bool asanUseGlobalsGC(const Triple &T, const CodeGenOptions &CGOpts) {
  if (!CGOpts.SanitizeAddressGlobalsDeadStripping)
    return false;
  switch (T.getObjectFormat()) {
  case Triple::MachO:
  case Triple::COFF:
    return true;
  case Triple::ELF:
    return CGOpts.DataSections && !CGOpts.DisableIntegratedAS;
  case Triple::XCOFF:
    llvm::report_fatal_error("ASan not implemented for XCOFF.");
  case Triple::Wasm:
  case Triple::UnknownObjectFormat:
    break;
  }
  return false;
}

static void addAddressSanitizerPasses(const PassManagerBuilder &Builder,
                                      legacy::PassManagerBase &PM) {
  const PassManagerBuilderWrapper &BuilderWrapper =
      static_cast<const PassManagerBuilderWrapper&>(Builder);
  const Triple &T = BuilderWrapper.getTargetTriple();
  const CodeGenOptions &CGOpts = BuilderWrapper.getCGOpts();
  bool Recover = CGOpts.SanitizeRecover.has(SanitizerKind::Address);
  bool UseAfterScope = CGOpts.SanitizeAddressUseAfterScope;
  bool UseOdrIndicator = CGOpts.SanitizeAddressUseOdrIndicator;
  bool UseGlobalsGC = asanUseGlobalsGC(T, CGOpts);
  PM.add(createAddressSanitizerFunctionPass(/*CompileKernel*/ false, Recover,
                                            UseAfterScope));
  PM.add(createModuleAddressSanitizerLegacyPassPass(
      /*CompileKernel*/ false, Recover, UseGlobalsGC, UseOdrIndicator));
}

static void addKernelAddressSanitizerPasses(const PassManagerBuilder &Builder,
                                            legacy::PassManagerBase &PM) {
  PM.add(createAddressSanitizerFunctionPass(
      /*CompileKernel*/ true, /*Recover*/ true, /*UseAfterScope*/ false));
  PM.add(createModuleAddressSanitizerLegacyPassPass(
      /*CompileKernel*/ true, /*Recover*/ true, /*UseGlobalsGC*/ true,
      /*UseOdrIndicator*/ false));
}

static void addHWAddressSanitizerPasses(const PassManagerBuilder &Builder,
                                            legacy::PassManagerBase &PM) {
  const PassManagerBuilderWrapper &BuilderWrapper =
      static_cast<const PassManagerBuilderWrapper &>(Builder);
  const CodeGenOptions &CGOpts = BuilderWrapper.getCGOpts();
  bool Recover = CGOpts.SanitizeRecover.has(SanitizerKind::HWAddress);
  PM.add(
      createHWAddressSanitizerLegacyPassPass(/*CompileKernel*/ false, Recover));
}

static void addKernelHWAddressSanitizerPasses(const PassManagerBuilder &Builder,
                                            legacy::PassManagerBase &PM) {
  PM.add(createHWAddressSanitizerLegacyPassPass(
      /*CompileKernel*/ true, /*Recover*/ true));
}

static void addGeneralOptsForMemorySanitizer(const PassManagerBuilder &Builder,
                                             legacy::PassManagerBase &PM,
                                             bool CompileKernel) {
  const PassManagerBuilderWrapper &BuilderWrapper =
      static_cast<const PassManagerBuilderWrapper&>(Builder);
  const CodeGenOptions &CGOpts = BuilderWrapper.getCGOpts();
  int TrackOrigins = CGOpts.SanitizeMemoryTrackOrigins;
  bool Recover = CGOpts.SanitizeRecover.has(SanitizerKind::Memory);
  PM.add(createMemorySanitizerLegacyPassPass(
      MemorySanitizerOptions{TrackOrigins, Recover, CompileKernel}));

  // MemorySanitizer inserts complex instrumentation that mostly follows
  // the logic of the original code, but operates on "shadow" values.
  // It can benefit from re-running some general purpose optimization passes.
  if (Builder.OptLevel > 0) {
    PM.add(createEarlyCSEPass());
    PM.add(createReassociatePass());
    PM.add(createLICMPass());
    PM.add(createGVNPass());
    PM.add(createInstructionCombiningPass());
    PM.add(createDeadStoreEliminationPass());
  }
}

static void addMemorySanitizerPass(const PassManagerBuilder &Builder,
                                   legacy::PassManagerBase &PM) {
  addGeneralOptsForMemorySanitizer(Builder, PM, /*CompileKernel*/ false);
}

static void addKernelMemorySanitizerPass(const PassManagerBuilder &Builder,
                                         legacy::PassManagerBase &PM) {
  addGeneralOptsForMemorySanitizer(Builder, PM, /*CompileKernel*/ true);
}

static void addThreadSanitizerPass(const PassManagerBuilder &Builder,
                                   legacy::PassManagerBase &PM) {
  PM.add(createThreadSanitizerLegacyPassPass());
}

static void addDataFlowSanitizerPass(const PassManagerBuilder &Builder,
                                     legacy::PassManagerBase &PM) {
  const PassManagerBuilderWrapper &BuilderWrapper =
      static_cast<const PassManagerBuilderWrapper&>(Builder);
  const LangOptions &LangOpts = BuilderWrapper.getLangOpts();
  PM.add(createDataFlowSanitizerPass(LangOpts.SanitizerBlacklistFiles));
}

static void addSoftPointerAuthPass(const PassManagerBuilder &Builder,
                                   legacy::PassManagerBase &PM) {
  PM.add(createSoftPointerAuthPass());
}

static TargetLibraryInfoImpl *createTLII(llvm::Triple &TargetTriple,
                                         const CodeGenOptions &CodeGenOpts) {
  TargetLibraryInfoImpl *TLII = new TargetLibraryInfoImpl(TargetTriple);

  switch (CodeGenOpts.getVecLib()) {
  case CodeGenOptions::Accelerate:
    TLII->addVectorizableFunctionsFromVecLib(TargetLibraryInfoImpl::Accelerate);
    break;
  case CodeGenOptions::MASSV:
    TLII->addVectorizableFunctionsFromVecLib(TargetLibraryInfoImpl::MASSV);
    break;
  case CodeGenOptions::SVML:
    TLII->addVectorizableFunctionsFromVecLib(TargetLibraryInfoImpl::SVML);
    break;
  default:
    break;
  }
  return TLII;
}

static void addSymbolRewriterPass(const CodeGenOptions &Opts,
                                  legacy::PassManager *MPM) {
  llvm::SymbolRewriter::RewriteDescriptorList DL;

  llvm::SymbolRewriter::RewriteMapParser MapParser;
  for (const auto &MapFile : Opts.RewriteMapFiles)
    MapParser.parse(MapFile, &DL);

  MPM->add(createRewriteSymbolsPass(DL));
}

static CodeGenOpt::Level getCGOptLevel(const CodeGenOptions &CodeGenOpts) {
  switch (CodeGenOpts.OptimizationLevel) {
  default:
    llvm_unreachable("Invalid optimization level!");
  case 0:
    return CodeGenOpt::None;
  case 1:
    return CodeGenOpt::Less;
  case 2:
    return CodeGenOpt::Default; // O2/Os/Oz
  case 3:
    return CodeGenOpt::Aggressive;
  }
}

static Optional<llvm::CodeModel::Model>
getCodeModel(const CodeGenOptions &CodeGenOpts) {
  unsigned CodeModel = llvm::StringSwitch<unsigned>(CodeGenOpts.CodeModel)
                           .Case("tiny", llvm::CodeModel::Tiny)
                           .Case("small", llvm::CodeModel::Small)
                           .Case("kernel", llvm::CodeModel::Kernel)
                           .Case("medium", llvm::CodeModel::Medium)
                           .Case("large", llvm::CodeModel::Large)
                           .Case("default", ~1u)
                           .Default(~0u);
  assert(CodeModel != ~0u && "invalid code model!");
  if (CodeModel == ~1u)
    return None;
  return static_cast<llvm::CodeModel::Model>(CodeModel);
}

static CodeGenFileType getCodeGenFileType(BackendAction Action) {
  if (Action == Backend_EmitObj)
    return CGFT_ObjectFile;
  else if (Action == Backend_EmitMCNull)
    return CGFT_Null;
  else {
    assert(Action == Backend_EmitAssembly && "Invalid action!");
    return CGFT_AssemblyFile;
  }
}

static void initTargetOptions(DiagnosticsEngine &Diags,
                              llvm::TargetOptions &Options,
                              const CodeGenOptions &CodeGenOpts,
                              const clang::TargetOptions &TargetOpts,
                              const LangOptions &LangOpts,
                              const HeaderSearchOptions &HSOpts) {
  Options.ThreadModel =
      llvm::StringSwitch<llvm::ThreadModel::Model>(CodeGenOpts.ThreadModel)
          .Case("posix", llvm::ThreadModel::POSIX)
          .Case("single", llvm::ThreadModel::Single);

  // Set float ABI type.
  assert((CodeGenOpts.FloatABI == "soft" || CodeGenOpts.FloatABI == "softfp" ||
          CodeGenOpts.FloatABI == "hard" || CodeGenOpts.FloatABI.empty()) &&
         "Invalid Floating Point ABI!");
  Options.FloatABIType =
      llvm::StringSwitch<llvm::FloatABI::ABIType>(CodeGenOpts.FloatABI)
          .Case("soft", llvm::FloatABI::Soft)
          .Case("softfp", llvm::FloatABI::Soft)
          .Case("hard", llvm::FloatABI::Hard)
          .Default(llvm::FloatABI::Default);

  // Set FP fusion mode.
  switch (LangOpts.getDefaultFPContractMode()) {
  case LangOptions::FPM_Off:
    // Preserve any contraction performed by the front-end.  (Strict performs
    // splitting of the muladd intrinsic in the backend.)
    Options.AllowFPOpFusion = llvm::FPOpFusion::Standard;
    break;
  case LangOptions::FPM_On:
    Options.AllowFPOpFusion = llvm::FPOpFusion::Standard;
    break;
  case LangOptions::FPM_Fast:
    Options.AllowFPOpFusion = llvm::FPOpFusion::Fast;
    break;
  }

  Options.UseInitArray = CodeGenOpts.UseInitArray;
  Options.DisableIntegratedAS = CodeGenOpts.DisableIntegratedAS;
  Options.CompressDebugSections = CodeGenOpts.getCompressDebugSections();
  Options.RelaxELFRelocations = CodeGenOpts.RelaxELFRelocations;

  // Set EABI version.
  Options.EABIVersion = TargetOpts.EABIVersion;

  if (LangOpts.SjLjExceptions)
    Options.ExceptionModel = llvm::ExceptionHandling::SjLj;
  if (LangOpts.SEHExceptions)
    Options.ExceptionModel = llvm::ExceptionHandling::WinEH;
  if (LangOpts.DWARFExceptions)
    Options.ExceptionModel = llvm::ExceptionHandling::DwarfCFI;
  if (LangOpts.WasmExceptions)
    Options.ExceptionModel = llvm::ExceptionHandling::Wasm;

  Options.NoInfsFPMath = LangOpts.NoHonorInfs;
  Options.NoNaNsFPMath = LangOpts.NoHonorNaNs;
  Options.NoZerosInBSS = CodeGenOpts.NoZeroInitializedInBSS;
  Options.UnsafeFPMath = LangOpts.UnsafeFPMath;
  Options.StackAlignmentOverride = CodeGenOpts.StackAlignment;

  Options.BBSections =
      llvm::StringSwitch<llvm::BasicBlockSection>(CodeGenOpts.BBSections)
          .Case("all", llvm::BasicBlockSection::All)
          .Case("labels", llvm::BasicBlockSection::Labels)
          .StartsWith("list=", llvm::BasicBlockSection::List)
          .Case("none", llvm::BasicBlockSection::None)
          .Default(llvm::BasicBlockSection::None);

  if (Options.BBSections == llvm::BasicBlockSection::List) {
    ErrorOr<std::unique_ptr<MemoryBuffer>> MBOrErr =
        MemoryBuffer::getFile(CodeGenOpts.BBSections.substr(5));
    if (!MBOrErr)
      Diags.Report(diag::err_fe_unable_to_load_basic_block_sections_file)
          << MBOrErr.getError().message();
    else
      Options.BBSectionsFuncListBuf = std::move(*MBOrErr);
  }

  Options.FunctionSections = CodeGenOpts.FunctionSections;
  Options.DataSections = CodeGenOpts.DataSections;
  Options.UniqueSectionNames = CodeGenOpts.UniqueSectionNames;
  Options.UniqueBasicBlockSectionNames =
      CodeGenOpts.UniqueBasicBlockSectionNames;
  Options.TLSSize = CodeGenOpts.TLSSize;
  Options.EmulatedTLS = CodeGenOpts.EmulatedTLS;
  Options.ExplicitEmulatedTLS = CodeGenOpts.ExplicitEmulatedTLS;
  Options.DebuggerTuning = CodeGenOpts.getDebuggerTuning();
  Options.EmitStackSizeSection = CodeGenOpts.StackSizeSection;
  Options.EmitAddrsig = CodeGenOpts.Addrsig;
  Options.ForceDwarfFrameSection = CodeGenOpts.ForceDwarfFrameSection;
  Options.EmitCallSiteInfo = CodeGenOpts.EmitCallSiteInfo;

  Options.MCOptions.SplitDwarfFile = CodeGenOpts.SplitDwarfFile;
  Options.MCOptions.MCRelaxAll = CodeGenOpts.RelaxAll;
  Options.MCOptions.MCSaveTempLabels = CodeGenOpts.SaveTempLabels;
  Options.MCOptions.MCUseDwarfDirectory = !CodeGenOpts.NoDwarfDirectoryAsm;
  Options.MCOptions.MCNoExecStack = CodeGenOpts.NoExecStack;
  Options.MCOptions.MCIncrementalLinkerCompatible =
      CodeGenOpts.IncrementalLinkerCompatible;
  Options.MCOptions.MCFatalWarnings = CodeGenOpts.FatalWarnings;
  Options.MCOptions.MCNoWarn = CodeGenOpts.NoWarn;
  Options.MCOptions.AsmVerbose = CodeGenOpts.AsmVerbose;
  Options.MCOptions.PreserveAsmComments = CodeGenOpts.PreserveAsmComments;
  Options.MCOptions.ABIName = TargetOpts.ABI;
  for (const auto &Entry : HSOpts.UserEntries)
    if (!Entry.IsFramework &&
        (Entry.Group == frontend::IncludeDirGroup::Quoted ||
         Entry.Group == frontend::IncludeDirGroup::Angled ||
         Entry.Group == frontend::IncludeDirGroup::System))
      Options.MCOptions.IASSearchPaths.push_back(
          Entry.IgnoreSysRoot ? Entry.Path : HSOpts.Sysroot + Entry.Path);
}
static Optional<GCOVOptions> getGCOVOptions(const CodeGenOptions &CodeGenOpts) {
  if (CodeGenOpts.DisableGCov)
    return None;
  if (!CodeGenOpts.EmitGcovArcs && !CodeGenOpts.EmitGcovNotes)
    return None;
  // Not using 'GCOVOptions::getDefault' allows us to avoid exiting if
  // LLVM's -default-gcov-version flag is set to something invalid.
  GCOVOptions Options;
  Options.EmitNotes = CodeGenOpts.EmitGcovNotes;
  Options.EmitData = CodeGenOpts.EmitGcovArcs;
  llvm::copy(CodeGenOpts.CoverageVersion, std::begin(Options.Version));
  Options.NoRedZone = CodeGenOpts.DisableRedZone;
  Options.Filter = CodeGenOpts.ProfileFilterFiles;
  Options.Exclude = CodeGenOpts.ProfileExcludeFiles;
  return Options;
}

static Optional<InstrProfOptions>
getInstrProfOptions(const CodeGenOptions &CodeGenOpts,
                    const LangOptions &LangOpts) {
  if (!CodeGenOpts.hasProfileClangInstr())
    return None;
  InstrProfOptions Options;
  Options.NoRedZone = CodeGenOpts.DisableRedZone;
  Options.InstrProfileOutput = CodeGenOpts.InstrProfileOutput;

  // TODO: Surface the option to emit atomic profile counter increments at
  // the driver level.
  Options.Atomic = LangOpts.Sanitize.has(SanitizerKind::Thread);
  return Options;
}

void EmitAssemblyHelper::CreatePasses(legacy::PassManager &MPM,
                                      legacy::FunctionPassManager &FPM) {
  // Handle disabling of all LLVM passes, where we want to preserve the
  // internal module before any optimization.
  if (CodeGenOpts.DisableLLVMPasses)
    return;

  // Figure out TargetLibraryInfo.  This needs to be added to MPM and FPM
  // manually (and not via PMBuilder), since some passes (eg. InstrProfiling)
  // are inserted before PMBuilder ones - they'd get the default-constructed
  // TLI with an unknown target otherwise.
  Triple TargetTriple(TheModule->getTargetTriple());
  std::unique_ptr<TargetLibraryInfoImpl> TLII(
      createTLII(TargetTriple, CodeGenOpts));

  // If we reached here with a non-empty index file name, then the index file
  // was empty and we are not performing ThinLTO backend compilation (used in
  // testing in a distributed build environment). Drop any the type test
  // assume sequences inserted for whole program vtables so that codegen doesn't
  // complain.
  if (!CodeGenOpts.ThinLTOIndexFile.empty())
    MPM.add(createLowerTypeTestsPass(/*ExportSummary=*/nullptr,
                                     /*ImportSummary=*/nullptr,
                                     /*DropTypeTests=*/true));

  PassManagerBuilderWrapper PMBuilder(TargetTriple, CodeGenOpts, LangOpts);

  // At O0 and O1 we only run the always inliner which is more efficient. At
  // higher optimization levels we run the normal inliner.
  if (CodeGenOpts.OptimizationLevel <= 1) {
    bool InsertLifetimeIntrinsics = ((CodeGenOpts.OptimizationLevel != 0 &&
                                      !CodeGenOpts.DisableLifetimeMarkers) ||
                                     LangOpts.Coroutines);
    PMBuilder.Inliner = createAlwaysInlinerLegacyPass(InsertLifetimeIntrinsics);
  } else {
    // We do not want to inline hot callsites for SamplePGO module-summary build
    // because profile annotation will happen again in ThinLTO backend, and we
    // want the IR of the hot path to match the profile.
    PMBuilder.Inliner = createFunctionInliningPass(
        CodeGenOpts.OptimizationLevel, CodeGenOpts.OptimizeSize,
        (!CodeGenOpts.SampleProfileFile.empty() &&
         CodeGenOpts.PrepareForThinLTO));
  }

  PMBuilder.OptLevel = CodeGenOpts.OptimizationLevel;
  PMBuilder.SizeLevel = CodeGenOpts.OptimizeSize;
  PMBuilder.SLPVectorize = CodeGenOpts.VectorizeSLP;
  PMBuilder.LoopVectorize = CodeGenOpts.VectorizeLoop;

  PMBuilder.DisableUnrollLoops = !CodeGenOpts.UnrollLoops;
  // Loop interleaving in the loop vectorizer has historically been set to be
  // enabled when loop unrolling is enabled.
  PMBuilder.LoopsInterleaved = CodeGenOpts.UnrollLoops;
  PMBuilder.MergeFunctions = CodeGenOpts.MergeFunctions;
  PMBuilder.SplitColdCode = CodeGenOpts.SplitColdCode;
  PMBuilder.PrepareForThinLTO = CodeGenOpts.PrepareForThinLTO;
  PMBuilder.PrepareForLTO = CodeGenOpts.PrepareForLTO;
  PMBuilder.RerollLoops = CodeGenOpts.RerollLoops;

  MPM.add(new TargetLibraryInfoWrapperPass(*TLII));

  if (TM)
    TM->adjustPassManager(PMBuilder);

  if (CodeGenOpts.DebugInfoForProfiling ||
      !CodeGenOpts.SampleProfileFile.empty())
    PMBuilder.addExtension(PassManagerBuilder::EP_EarlyAsPossible,
                           addAddDiscriminatorsPass);

  // In ObjC ARC mode, add the main ARC optimization passes.
  if (LangOpts.ObjCAutoRefCount) {
    PMBuilder.addExtension(PassManagerBuilder::EP_EarlyAsPossible,
                           addObjCARCExpandPass);
    PMBuilder.addExtension(PassManagerBuilder::EP_ModuleOptimizerEarly,
                           addObjCARCAPElimPass);
    PMBuilder.addExtension(PassManagerBuilder::EP_ScalarOptimizerLate,
                           addObjCARCOptPass);
  }

  if (LangOpts.Coroutines)
    addCoroutinePassesToExtensionPoints(PMBuilder);

  if (LangOpts.Sanitize.has(SanitizerKind::LocalBounds)) {
    PMBuilder.addExtension(PassManagerBuilder::EP_ScalarOptimizerLate,
                           addBoundsCheckingPass);
    PMBuilder.addExtension(PassManagerBuilder::EP_EnabledOnOptLevel0,
                           addBoundsCheckingPass);
  }

  if (CodeGenOpts.SanitizeCoverageType ||
      CodeGenOpts.SanitizeCoverageIndirectCalls ||
      CodeGenOpts.SanitizeCoverageTraceCmp) {
    PMBuilder.addExtension(PassManagerBuilder::EP_OptimizerLast,
                           addSanitizerCoveragePass);
    PMBuilder.addExtension(PassManagerBuilder::EP_EnabledOnOptLevel0,
                           addSanitizerCoveragePass);
  }

  if (LangOpts.Sanitize.has(SanitizerKind::Address)) {
    PMBuilder.addExtension(PassManagerBuilder::EP_OptimizerLast,
                           addAddressSanitizerPasses);
    PMBuilder.addExtension(PassManagerBuilder::EP_EnabledOnOptLevel0,
                           addAddressSanitizerPasses);
  }

  if (LangOpts.Sanitize.has(SanitizerKind::KernelAddress)) {
    PMBuilder.addExtension(PassManagerBuilder::EP_OptimizerLast,
                           addKernelAddressSanitizerPasses);
    PMBuilder.addExtension(PassManagerBuilder::EP_EnabledOnOptLevel0,
                           addKernelAddressSanitizerPasses);
  }

  if (LangOpts.Sanitize.has(SanitizerKind::HWAddress)) {
    PMBuilder.addExtension(PassManagerBuilder::EP_OptimizerLast,
                           addHWAddressSanitizerPasses);
    PMBuilder.addExtension(PassManagerBuilder::EP_EnabledOnOptLevel0,
                           addHWAddressSanitizerPasses);
  }

  if (LangOpts.Sanitize.has(SanitizerKind::KernelHWAddress)) {
    PMBuilder.addExtension(PassManagerBuilder::EP_OptimizerLast,
                           addKernelHWAddressSanitizerPasses);
    PMBuilder.addExtension(PassManagerBuilder::EP_EnabledOnOptLevel0,
                           addKernelHWAddressSanitizerPasses);
  }

  if (LangOpts.Sanitize.has(SanitizerKind::Memory)) {
    PMBuilder.addExtension(PassManagerBuilder::EP_OptimizerLast,
                           addMemorySanitizerPass);
    PMBuilder.addExtension(PassManagerBuilder::EP_EnabledOnOptLevel0,
                           addMemorySanitizerPass);
  }

  if (LangOpts.Sanitize.has(SanitizerKind::KernelMemory)) {
    PMBuilder.addExtension(PassManagerBuilder::EP_OptimizerLast,
                           addKernelMemorySanitizerPass);
    PMBuilder.addExtension(PassManagerBuilder::EP_EnabledOnOptLevel0,
                           addKernelMemorySanitizerPass);
  }

  if (LangOpts.Sanitize.has(SanitizerKind::Thread)) {
    PMBuilder.addExtension(PassManagerBuilder::EP_OptimizerLast,
                           addThreadSanitizerPass);
    PMBuilder.addExtension(PassManagerBuilder::EP_EnabledOnOptLevel0,
                           addThreadSanitizerPass);
  }

  if (LangOpts.Sanitize.has(SanitizerKind::DataFlow)) {
    PMBuilder.addExtension(PassManagerBuilder::EP_OptimizerLast,
                           addDataFlowSanitizerPass);
    PMBuilder.addExtension(PassManagerBuilder::EP_EnabledOnOptLevel0,
                           addDataFlowSanitizerPass);
  }

  if (LangOpts.SoftPointerAuth) {
    PMBuilder.addExtension(PassManagerBuilder::EP_OptimizerLast,
                           addSoftPointerAuthPass);
    PMBuilder.addExtension(PassManagerBuilder::EP_EnabledOnOptLevel0,
                           addSoftPointerAuthPass);
  }

  // Set up the per-function pass manager.
  FPM.add(new TargetLibraryInfoWrapperPass(*TLII));
  if (CodeGenOpts.VerifyModule)
    FPM.add(createVerifierPass());

  // Set up the per-module pass manager.
  if (!CodeGenOpts.RewriteMapFiles.empty())
    addSymbolRewriterPass(CodeGenOpts, &MPM);

  // Add UniqueInternalLinkageNames Pass which renames internal linkage symbols
  // with unique names.
  if (CodeGenOpts.UniqueInternalLinkageNames) {
    MPM.add(createUniqueInternalLinkageNamesPass());
  }

  if (Optional<GCOVOptions> Options = getGCOVOptions(CodeGenOpts)) {
    MPM.add(createGCOVProfilerPass(*Options));
    if (CodeGenOpts.getDebugInfo() == codegenoptions::NoDebugInfo)
      MPM.add(createStripSymbolsPass(true));
  }

  if (Optional<InstrProfOptions> Options =
          getInstrProfOptions(CodeGenOpts, LangOpts))
    MPM.add(createInstrProfilingLegacyPass(*Options, false));

  bool hasIRInstr = false;
  if (CodeGenOpts.hasProfileIRInstr()) {
    PMBuilder.EnablePGOInstrGen = true;
    hasIRInstr = true;
  }
  if (CodeGenOpts.hasProfileCSIRInstr()) {
    assert(!CodeGenOpts.hasProfileCSIRUse() &&
           "Cannot have both CSProfileUse pass and CSProfileGen pass at the "
           "same time");
    assert(!hasIRInstr &&
           "Cannot have both ProfileGen pass and CSProfileGen pass at the "
           "same time");
    PMBuilder.EnablePGOCSInstrGen = true;
    hasIRInstr = true;
  }
  if (hasIRInstr) {
    if (!CodeGenOpts.InstrProfileOutput.empty())
      PMBuilder.PGOInstrGen = CodeGenOpts.InstrProfileOutput;
    else
      PMBuilder.PGOInstrGen = std::string(DefaultProfileGenName);
  }
  if (CodeGenOpts.hasProfileIRUse()) {
    PMBuilder.PGOInstrUse = CodeGenOpts.ProfileInstrumentUsePath;
    PMBuilder.EnablePGOCSInstrUse = CodeGenOpts.hasProfileCSIRUse();
  }

  if (!CodeGenOpts.SampleProfileFile.empty())
    PMBuilder.PGOSampleUse = CodeGenOpts.SampleProfileFile;

  PMBuilder.populateFunctionPassManager(FPM);
  PMBuilder.populateModulePassManager(MPM);
}

static void setCommandLineOpts(const CodeGenOptions &CodeGenOpts) {
  SmallVector<const char *, 16> BackendArgs;
  BackendArgs.push_back("clang"); // Fake program name.
  if (!CodeGenOpts.DebugPass.empty()) {
    BackendArgs.push_back("-debug-pass");
    BackendArgs.push_back(CodeGenOpts.DebugPass.c_str());
  }
  if (!CodeGenOpts.LimitFloatPrecision.empty()) {
    BackendArgs.push_back("-limit-float-precision");
    BackendArgs.push_back(CodeGenOpts.LimitFloatPrecision.c_str());
  }
  BackendArgs.push_back(nullptr);
  llvm::cl::ParseCommandLineOptions(BackendArgs.size() - 1,
                                    BackendArgs.data());
}

void EmitAssemblyHelper::CreateTargetMachine(bool MustCreateTM) {
  // Create the TargetMachine for generating code.
  std::string Error;
  std::string Triple = TheModule->getTargetTriple();
  const llvm::Target *TheTarget = TargetRegistry::lookupTarget(Triple, Error);
  if (!TheTarget) {
    if (MustCreateTM)
      Diags.Report(diag::err_fe_unable_to_create_target) << Error;
    return;
  }

  Optional<llvm::CodeModel::Model> CM = getCodeModel(CodeGenOpts);
  std::string FeaturesStr =
      llvm::join(TargetOpts.Features.begin(), TargetOpts.Features.end(), ",");
  llvm::Reloc::Model RM = CodeGenOpts.RelocationModel;
  CodeGenOpt::Level OptLevel = getCGOptLevel(CodeGenOpts);

  llvm::TargetOptions Options;
  initTargetOptions(Diags, Options, CodeGenOpts, TargetOpts, LangOpts, HSOpts);
  TM.reset(TheTarget->createTargetMachine(Triple, TargetOpts.CPU, FeaturesStr,
                                          Options, RM, CM, OptLevel));
}

bool EmitAssemblyHelper::AddEmitPasses(legacy::PassManager &CodeGenPasses,
                                       BackendAction Action,
                                       raw_pwrite_stream &OS,
                                       raw_pwrite_stream *DwoOS) {
  // Add LibraryInfo.
  llvm::Triple TargetTriple(TheModule->getTargetTriple());
  std::unique_ptr<TargetLibraryInfoImpl> TLII(
      createTLII(TargetTriple, CodeGenOpts));
  CodeGenPasses.add(new TargetLibraryInfoWrapperPass(*TLII));

  // Normal mode, emit a .s or .o file by running the code generator. Note,
  // this also adds codegenerator level optimization passes.
  CodeGenFileType CGFT = getCodeGenFileType(Action);

  // Add ObjC ARC final-cleanup optimizations. This is done as part of the
  // "codegen" passes so that it isn't run multiple times when there is
  // inlining happening.
  if (CodeGenOpts.OptimizationLevel > 0)
    CodeGenPasses.add(createObjCARCContractPass());

  if (TM->addPassesToEmitFile(CodeGenPasses, OS, DwoOS, CGFT,
                              /*DisableVerify=*/!CodeGenOpts.VerifyModule)) {
    Diags.Report(diag::err_fe_unable_to_interface_with_target);
    return false;
  }

  return true;
}

void EmitAssemblyHelper::EmitAssembly(BackendAction Action,
                                      std::unique_ptr<raw_pwrite_stream> OS) {
  TimeRegion Region(FrontendTimesIsEnabled ? &CodeGenerationTime : nullptr);

  setCommandLineOpts(CodeGenOpts);

  bool UsesCodeGen = (Action != Backend_EmitNothing &&
                      Action != Backend_EmitBC &&
                      Action != Backend_EmitLL);
  CreateTargetMachine(UsesCodeGen);

  if (UsesCodeGen && !TM)
    return;
  if (TM)
    TheModule->setDataLayout(TM->createDataLayout());

  legacy::PassManager PerModulePasses;
  PerModulePasses.add(
      createTargetTransformInfoWrapperPass(getTargetIRAnalysis()));

  legacy::FunctionPassManager PerFunctionPasses(TheModule);
  PerFunctionPasses.add(
      createTargetTransformInfoWrapperPass(getTargetIRAnalysis()));

  CreatePasses(PerModulePasses, PerFunctionPasses);

  legacy::PassManager CodeGenPasses;
  CodeGenPasses.add(
      createTargetTransformInfoWrapperPass(getTargetIRAnalysis()));

  std::unique_ptr<llvm::ToolOutputFile> ThinLinkOS, DwoOS;

  switch (Action) {
  case Backend_EmitNothing:
    break;

  case Backend_EmitBC:
    if (CodeGenOpts.PrepareForThinLTO && !CodeGenOpts.DisableLLVMPasses) {
      if (!CodeGenOpts.ThinLinkBitcodeFile.empty()) {
        ThinLinkOS = openOutputFile(CodeGenOpts.ThinLinkBitcodeFile);
        if (!ThinLinkOS)
          return;
      }
      TheModule->addModuleFlag(Module::Error, "EnableSplitLTOUnit",
                               CodeGenOpts.EnableSplitLTOUnit);
      PerModulePasses.add(createWriteThinLTOBitcodePass(
          *OS, ThinLinkOS ? &ThinLinkOS->os() : nullptr));
    } else {
      // Emit a module summary by default for Regular LTO except for ld64
      // targets
      bool EmitLTOSummary =
          (CodeGenOpts.PrepareForLTO &&
           !CodeGenOpts.DisableLLVMPasses &&
           llvm::Triple(TheModule->getTargetTriple()).getVendor() !=
               llvm::Triple::Apple);
      if (EmitLTOSummary) {
        if (!TheModule->getModuleFlag("ThinLTO"))
          TheModule->addModuleFlag(Module::Error, "ThinLTO", uint32_t(0));
        TheModule->addModuleFlag(Module::Error, "EnableSplitLTOUnit",
                                 uint32_t(1));
      }

      PerModulePasses.add(createBitcodeWriterPass(
          *OS, CodeGenOpts.EmitLLVMUseLists, EmitLTOSummary));
    }
    break;

  case Backend_EmitLL:
    PerModulePasses.add(
        createPrintModulePass(*OS, "", CodeGenOpts.EmitLLVMUseLists));
    break;

  default:
    if (!CodeGenOpts.SplitDwarfOutput.empty()) {
      DwoOS = openOutputFile(CodeGenOpts.SplitDwarfOutput);
      if (!DwoOS)
        return;
    }
    if (!AddEmitPasses(CodeGenPasses, Action, *OS,
                       DwoOS ? &DwoOS->os() : nullptr))
      return;
  }

  // Before executing passes, print the final values of the LLVM options.
  cl::PrintOptionValues();

  // Run passes. For now we do all passes at once, but eventually we
  // would like to have the option of streaming code generation.

  {
    PrettyStackTraceString CrashInfo("Per-function optimization");
    llvm::TimeTraceScope TimeScope("PerFunctionPasses");

    PerFunctionPasses.doInitialization();
    for (Function &F : *TheModule)
      if (!F.isDeclaration())
        PerFunctionPasses.run(F);
    PerFunctionPasses.doFinalization();
  }

  {
    PrettyStackTraceString CrashInfo("Per-module optimization passes");
    llvm::TimeTraceScope TimeScope("PerModulePasses");
    PerModulePasses.run(*TheModule);
  }

  {
    PrettyStackTraceString CrashInfo("Code generation");
    llvm::TimeTraceScope TimeScope("CodeGenPasses");
    CodeGenPasses.run(*TheModule);
  }

  if (ThinLinkOS)
    ThinLinkOS->keep();
  if (DwoOS)
    DwoOS->keep();
}

static PassBuilder::OptimizationLevel mapToLevel(const CodeGenOptions &Opts) {
  switch (Opts.OptimizationLevel) {
  default:
    llvm_unreachable("Invalid optimization level!");

  case 1:
    return PassBuilder::OptimizationLevel::O1;

  case 2:
    switch (Opts.OptimizeSize) {
    default:
      llvm_unreachable("Invalid optimization level for size!");

    case 0:
      return PassBuilder::OptimizationLevel::O2;

    case 1:
      return PassBuilder::OptimizationLevel::Os;

    case 2:
      return PassBuilder::OptimizationLevel::Oz;
    }

  case 3:
    return PassBuilder::OptimizationLevel::O3;
  }
}

static void addCoroutinePassesAtO0(ModulePassManager &MPM,
                                   const LangOptions &LangOpts,
                                   const CodeGenOptions &CodeGenOpts) {
  if (!LangOpts.Coroutines)
    return;

  MPM.addPass(createModuleToFunctionPassAdaptor(CoroEarlyPass()));

  CGSCCPassManager CGPM(CodeGenOpts.DebugPassManager);
  CGPM.addPass(CoroSplitPass());
  CGPM.addPass(createCGSCCToFunctionPassAdaptor(CoroElidePass()));
  MPM.addPass(createModuleToPostOrderCGSCCPassAdaptor(std::move(CGPM)));

  MPM.addPass(createModuleToFunctionPassAdaptor(CoroCleanupPass()));
}

static void addSanitizersAtO0(ModulePassManager &MPM,
                              const Triple &TargetTriple,
                              const LangOptions &LangOpts,
                              const CodeGenOptions &CodeGenOpts) {
  if (CodeGenOpts.SanitizeCoverageType ||
      CodeGenOpts.SanitizeCoverageIndirectCalls ||
      CodeGenOpts.SanitizeCoverageTraceCmp) {
    auto SancovOpts = getSancovOptsFromCGOpts(CodeGenOpts);
    MPM.addPass(ModuleSanitizerCoveragePass(
        SancovOpts, CodeGenOpts.SanitizeCoverageWhitelistFiles,
        CodeGenOpts.SanitizeCoverageBlacklistFiles));
  }

  auto ASanPass = [&](SanitizerMask Mask, bool CompileKernel) {
    MPM.addPass(RequireAnalysisPass<ASanGlobalsMetadataAnalysis, Module>());
    bool Recover = CodeGenOpts.SanitizeRecover.has(Mask);
    MPM.addPass(createModuleToFunctionPassAdaptor(AddressSanitizerPass(
        CompileKernel, Recover, CodeGenOpts.SanitizeAddressUseAfterScope)));
    bool ModuleUseAfterScope = asanUseGlobalsGC(TargetTriple, CodeGenOpts);
    MPM.addPass(
        ModuleAddressSanitizerPass(CompileKernel, Recover, ModuleUseAfterScope,
                                   CodeGenOpts.SanitizeAddressUseOdrIndicator));
  };

  if (LangOpts.Sanitize.has(SanitizerKind::Address)) {
    ASanPass(SanitizerKind::Address, /*CompileKernel=*/false);
  }

  if (LangOpts.Sanitize.has(SanitizerKind::KernelAddress)) {
    ASanPass(SanitizerKind::KernelAddress, /*CompileKernel=*/true);
  }

  if (LangOpts.Sanitize.has(SanitizerKind::Memory)) {
    bool Recover = CodeGenOpts.SanitizeRecover.has(SanitizerKind::Memory);
    int TrackOrigins = CodeGenOpts.SanitizeMemoryTrackOrigins;
    MPM.addPass(MemorySanitizerPass({TrackOrigins, Recover, false}));
    MPM.addPass(createModuleToFunctionPassAdaptor(
        MemorySanitizerPass({TrackOrigins, Recover, false})));
  }

  if (LangOpts.Sanitize.has(SanitizerKind::KernelMemory)) {
    MPM.addPass(createModuleToFunctionPassAdaptor(
        MemorySanitizerPass({0, false, /*Kernel=*/true})));
  }

  if (LangOpts.Sanitize.has(SanitizerKind::Thread)) {
    MPM.addPass(ThreadSanitizerPass());
    MPM.addPass(createModuleToFunctionPassAdaptor(ThreadSanitizerPass()));
  }
}

/// A clean version of `EmitAssembly` that uses the new pass manager.
///
/// Not all features are currently supported in this system, but where
/// necessary it falls back to the legacy pass manager to at least provide
/// basic functionality.
///
/// This API is planned to have its functionality finished and then to replace
/// `EmitAssembly` at some point in the future when the default switches.
void EmitAssemblyHelper::EmitAssemblyWithNewPassManager(
    BackendAction Action, std::unique_ptr<raw_pwrite_stream> OS) {
  TimeRegion Region(FrontendTimesIsEnabled ? &CodeGenerationTime : nullptr);
  setCommandLineOpts(CodeGenOpts);

  bool RequiresCodeGen = (Action != Backend_EmitNothing &&
                          Action != Backend_EmitBC &&
                          Action != Backend_EmitLL);
  CreateTargetMachine(RequiresCodeGen);

  if (RequiresCodeGen && !TM)
    return;
  if (TM)
    TheModule->setDataLayout(TM->createDataLayout());

  Optional<PGOOptions> PGOOpt;

  if (CodeGenOpts.hasProfileIRInstr())
    // -fprofile-generate.
    PGOOpt = PGOOptions(CodeGenOpts.InstrProfileOutput.empty()
                            ? std::string(DefaultProfileGenName)
                            : CodeGenOpts.InstrProfileOutput,
                        "", "", PGOOptions::IRInstr, PGOOptions::NoCSAction,
                        CodeGenOpts.DebugInfoForProfiling);
  else if (CodeGenOpts.hasProfileIRUse()) {
    // -fprofile-use.
    auto CSAction = CodeGenOpts.hasProfileCSIRUse() ? PGOOptions::CSIRUse
                                                    : PGOOptions::NoCSAction;
    PGOOpt = PGOOptions(CodeGenOpts.ProfileInstrumentUsePath, "",
                        CodeGenOpts.ProfileRemappingFile, PGOOptions::IRUse,
                        CSAction, CodeGenOpts.DebugInfoForProfiling);
  } else if (!CodeGenOpts.SampleProfileFile.empty())
    // -fprofile-sample-use
    PGOOpt =
        PGOOptions(CodeGenOpts.SampleProfileFile, "",
                   CodeGenOpts.ProfileRemappingFile, PGOOptions::SampleUse,
                   PGOOptions::NoCSAction, CodeGenOpts.DebugInfoForProfiling);
  else if (CodeGenOpts.DebugInfoForProfiling)
    // -fdebug-info-for-profiling
    PGOOpt = PGOOptions("", "", "", PGOOptions::NoAction,
                        PGOOptions::NoCSAction, true);

  // Check to see if we want to generate a CS profile.
  if (CodeGenOpts.hasProfileCSIRInstr()) {
    assert(!CodeGenOpts.hasProfileCSIRUse() &&
           "Cannot have both CSProfileUse pass and CSProfileGen pass at "
           "the same time");
    if (PGOOpt.hasValue()) {
      assert(PGOOpt->Action != PGOOptions::IRInstr &&
             PGOOpt->Action != PGOOptions::SampleUse &&
             "Cannot run CSProfileGen pass with ProfileGen or SampleUse "
             " pass");
      PGOOpt->CSProfileGenFile = CodeGenOpts.InstrProfileOutput.empty()
                                     ? std::string(DefaultProfileGenName)
                                     : CodeGenOpts.InstrProfileOutput;
      PGOOpt->CSAction = PGOOptions::CSIRInstr;
    } else
      PGOOpt = PGOOptions("",
                          CodeGenOpts.InstrProfileOutput.empty()
                              ? std::string(DefaultProfileGenName)
                              : CodeGenOpts.InstrProfileOutput,
                          "", PGOOptions::NoAction, PGOOptions::CSIRInstr,
                          CodeGenOpts.DebugInfoForProfiling);
  }

  PipelineTuningOptions PTO;
  PTO.LoopUnrolling = CodeGenOpts.UnrollLoops;
  // For historical reasons, loop interleaving is set to mirror setting for loop
  // unrolling.
  PTO.LoopInterleaving = CodeGenOpts.UnrollLoops;
  PTO.LoopVectorization = CodeGenOpts.VectorizeLoop;
  PTO.SLPVectorization = CodeGenOpts.VectorizeSLP;
  PTO.CallGraphProfile = CodeGenOpts.CallGraphProfile;
  PTO.Coroutines = LangOpts.Coroutines;

  PassInstrumentationCallbacks PIC;
  StandardInstrumentations SI;
  SI.registerCallbacks(PIC);
  PassBuilder PB(TM.get(), PTO, PGOOpt, &PIC);

  // Attempt to load pass plugins and register their callbacks with PB.
  for (auto &PluginFN : CodeGenOpts.PassPlugins) {
    auto PassPlugin = PassPlugin::Load(PluginFN);
    if (PassPlugin) {
      PassPlugin->registerPassBuilderCallbacks(PB);
    } else {
      Diags.Report(diag::err_fe_unable_to_load_plugin)
          << PluginFN << toString(PassPlugin.takeError());
    }
  }
#define HANDLE_EXTENSION(Ext)                                                  \
  get##Ext##PluginInfo().RegisterPassBuilderCallbacks(PB);
#include "llvm/Support/Extension.def"

  LoopAnalysisManager LAM(CodeGenOpts.DebugPassManager);
  FunctionAnalysisManager FAM(CodeGenOpts.DebugPassManager);
  CGSCCAnalysisManager CGAM(CodeGenOpts.DebugPassManager);
  ModuleAnalysisManager MAM(CodeGenOpts.DebugPassManager);

  // Register the AA manager first so that our version is the one used.
  FAM.registerPass([&] { return PB.buildDefaultAAPipeline(); });

  // Register the target library analysis directly and give it a customized
  // preset TLI.
  Triple TargetTriple(TheModule->getTargetTriple());
  std::unique_ptr<TargetLibraryInfoImpl> TLII(
      createTLII(TargetTriple, CodeGenOpts));
  FAM.registerPass([&] { return TargetLibraryAnalysis(*TLII); });

  // Register all the basic analyses with the managers.
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  ModulePassManager MPM(CodeGenOpts.DebugPassManager);

  if (!CodeGenOpts.DisableLLVMPasses) {
    bool IsThinLTO = CodeGenOpts.PrepareForThinLTO;
    bool IsLTO = CodeGenOpts.PrepareForLTO;

    if (CodeGenOpts.OptimizationLevel == 0) {
      // If we reached here with a non-empty index file name, then the index
      // file was empty and we are not performing ThinLTO backend compilation
      // (used in testing in a distributed build environment). Drop any the type
      // test assume sequences inserted for whole program vtables so that
      // codegen doesn't complain.
      if (!CodeGenOpts.ThinLTOIndexFile.empty())
        MPM.addPass(LowerTypeTestsPass(/*ExportSummary=*/nullptr,
                                       /*ImportSummary=*/nullptr,
                                       /*DropTypeTests=*/true));
      if (Optional<GCOVOptions> Options = getGCOVOptions(CodeGenOpts))
        MPM.addPass(GCOVProfilerPass(*Options));
      if (Optional<InstrProfOptions> Options =
              getInstrProfOptions(CodeGenOpts, LangOpts))
        MPM.addPass(InstrProfiling(*Options, false));

      // Build a minimal pipeline based on the semantics required by Clang,
      // which is just that always inlining occurs. Further, disable generating
      // lifetime intrinsics to avoid enabling further optimizations during
      // code generation.
      // However, we need to insert lifetime intrinsics to avoid invalid access
      // caused by multithreaded coroutines.
      MPM.addPass(
          AlwaysInlinerPass(/*InsertLifetimeIntrinsics=*/LangOpts.Coroutines));

      // At -O0, we can still do PGO. Add all the requested passes for
      // instrumentation PGO, if requested.
      if (PGOOpt && (PGOOpt->Action == PGOOptions::IRInstr ||
                     PGOOpt->Action == PGOOptions::IRUse))
        PB.addPGOInstrPassesForO0(
            MPM, CodeGenOpts.DebugPassManager,
            /* RunProfileGen */ (PGOOpt->Action == PGOOptions::IRInstr),
            /* IsCS */ false, PGOOpt->ProfileFile,
            PGOOpt->ProfileRemappingFile);

      // At -O0 we directly run necessary sanitizer passes.
      if (LangOpts.Sanitize.has(SanitizerKind::LocalBounds))
        MPM.addPass(createModuleToFunctionPassAdaptor(BoundsCheckingPass()));

      // Add UniqueInternalLinkageNames Pass which renames internal linkage
      // symbols with unique names.
      if (CodeGenOpts.UniqueInternalLinkageNames) {
        MPM.addPass(UniqueInternalLinkageNamesPass());
      }

      // Lastly, add semantically necessary passes for LTO.
      if (IsLTO || IsThinLTO) {
        MPM.addPass(CanonicalizeAliasesPass());
        MPM.addPass(NameAnonGlobalPass());
      }
    } else {
      // Map our optimization levels into one of the distinct levels used to
      // configure the pipeline.
      PassBuilder::OptimizationLevel Level = mapToLevel(CodeGenOpts);

      // -f[no-]split-cold-code
      PB.setEnableHotColdSplitting(CodeGenOpts.SplitColdCode);

      // If we reached here with a non-empty index file name, then the index
      // file was empty and we are not performing ThinLTO backend compilation
      // (used in testing in a distributed build environment). Drop any the type
      // test assume sequences inserted for whole program vtables so that
      // codegen doesn't complain.
      if (!CodeGenOpts.ThinLTOIndexFile.empty())
        PB.registerPipelineStartEPCallback([](ModulePassManager &MPM) {
          MPM.addPass(LowerTypeTestsPass(/*ExportSummary=*/nullptr,
                                         /*ImportSummary=*/nullptr,
                                         /*DropTypeTests=*/true));
        });

      PB.registerPipelineStartEPCallback([](ModulePassManager &MPM) {
        MPM.addPass(createModuleToFunctionPassAdaptor(
            EntryExitInstrumenterPass(/*PostInlining=*/false)));
      });

      // Register callbacks to schedule sanitizer passes at the appropriate part of
      // the pipeline.
      // FIXME: either handle asan/the remaining sanitizers or error out
      if (LangOpts.Sanitize.has(SanitizerKind::LocalBounds))
        PB.registerScalarOptimizerLateEPCallback(
            [](FunctionPassManager &FPM, PassBuilder::OptimizationLevel Level) {
              FPM.addPass(BoundsCheckingPass());
            });

      if (CodeGenOpts.SanitizeCoverageType ||
          CodeGenOpts.SanitizeCoverageIndirectCalls ||
          CodeGenOpts.SanitizeCoverageTraceCmp) {
        PB.registerOptimizerLastEPCallback(
            [this](ModulePassManager &MPM,
                   PassBuilder::OptimizationLevel Level) {
              auto SancovOpts = getSancovOptsFromCGOpts(CodeGenOpts);
              MPM.addPass(ModuleSanitizerCoveragePass(
                  SancovOpts, CodeGenOpts.SanitizeCoverageWhitelistFiles,
                  CodeGenOpts.SanitizeCoverageBlacklistFiles));
            });
      }

      if (LangOpts.Sanitize.has(SanitizerKind::Memory)) {
        int TrackOrigins = CodeGenOpts.SanitizeMemoryTrackOrigins;
        bool Recover = CodeGenOpts.SanitizeRecover.has(SanitizerKind::Memory);
        PB.registerPipelineStartEPCallback(
            [TrackOrigins, Recover](ModulePassManager &MPM) {
              MPM.addPass(MemorySanitizerPass({TrackOrigins, Recover, false}));
            });
        PB.registerOptimizerLastEPCallback(
            [TrackOrigins, Recover](ModulePassManager &MPM,
                                    PassBuilder::OptimizationLevel Level) {
              MPM.addPass(createModuleToFunctionPassAdaptor(
                  MemorySanitizerPass({TrackOrigins, Recover, false})));
            });
      }
      if (LangOpts.Sanitize.has(SanitizerKind::Thread)) {
        PB.registerPipelineStartEPCallback(
            [](ModulePassManager &MPM) { MPM.addPass(ThreadSanitizerPass()); });
        PB.registerOptimizerLastEPCallback(
            [](ModulePassManager &MPM, PassBuilder::OptimizationLevel Level) {
              MPM.addPass(
                  createModuleToFunctionPassAdaptor(ThreadSanitizerPass()));
            });
      }
      if (LangOpts.Sanitize.has(SanitizerKind::Address)) {
        PB.registerPipelineStartEPCallback([&](ModulePassManager &MPM) {
          MPM.addPass(
              RequireAnalysisPass<ASanGlobalsMetadataAnalysis, Module>());
        });
        bool Recover = CodeGenOpts.SanitizeRecover.has(SanitizerKind::Address);
        bool UseAfterScope = CodeGenOpts.SanitizeAddressUseAfterScope;
        PB.registerOptimizerLastEPCallback(
            [Recover, UseAfterScope](ModulePassManager &MPM,
                                     PassBuilder::OptimizationLevel Level) {
              MPM.addPass(
                  createModuleToFunctionPassAdaptor(AddressSanitizerPass(
                      /*CompileKernel=*/false, Recover, UseAfterScope)));
            });
        bool ModuleUseAfterScope = asanUseGlobalsGC(TargetTriple, CodeGenOpts);
        bool UseOdrIndicator = CodeGenOpts.SanitizeAddressUseOdrIndicator;
        PB.registerPipelineStartEPCallback(
            [Recover, ModuleUseAfterScope,
             UseOdrIndicator](ModulePassManager &MPM) {
              MPM.addPass(ModuleAddressSanitizerPass(
                  /*CompileKernel=*/false, Recover, ModuleUseAfterScope,
                  UseOdrIndicator));
            });
      }
      if (Optional<GCOVOptions> Options = getGCOVOptions(CodeGenOpts))
        PB.registerPipelineStartEPCallback([Options](ModulePassManager &MPM) {
          MPM.addPass(GCOVProfilerPass(*Options));
        });
      if (Optional<InstrProfOptions> Options =
              getInstrProfOptions(CodeGenOpts, LangOpts))
        PB.registerPipelineStartEPCallback([Options](ModulePassManager &MPM) {
          MPM.addPass(InstrProfiling(*Options, false));
        });

      // Add UniqueInternalLinkageNames Pass which renames internal linkage
      // symbols with unique names.
      if (CodeGenOpts.UniqueInternalLinkageNames) {
        MPM.addPass(UniqueInternalLinkageNamesPass());
      }

      if (IsThinLTO) {
        MPM = PB.buildThinLTOPreLinkDefaultPipeline(
            Level, CodeGenOpts.DebugPassManager);
        MPM.addPass(CanonicalizeAliasesPass());
        MPM.addPass(NameAnonGlobalPass());
      } else if (IsLTO) {
        MPM = PB.buildLTOPreLinkDefaultPipeline(Level,
                                                CodeGenOpts.DebugPassManager);
        MPM.addPass(CanonicalizeAliasesPass());
        MPM.addPass(NameAnonGlobalPass());
      } else {
        MPM = PB.buildPerModuleDefaultPipeline(Level,
                                               CodeGenOpts.DebugPassManager);
      }
    }

    if (LangOpts.Sanitize.has(SanitizerKind::HWAddress)) {
      bool Recover = CodeGenOpts.SanitizeRecover.has(SanitizerKind::HWAddress);
      MPM.addPass(HWAddressSanitizerPass(
          /*CompileKernel=*/false, Recover));
    }
    if (LangOpts.Sanitize.has(SanitizerKind::KernelHWAddress)) {
      MPM.addPass(HWAddressSanitizerPass(
          /*CompileKernel=*/true, /*Recover=*/true));
    }

    if (CodeGenOpts.OptimizationLevel == 0) {
      addCoroutinePassesAtO0(MPM, LangOpts, CodeGenOpts);
      addSanitizersAtO0(MPM, TargetTriple, LangOpts, CodeGenOpts);
    }
  }

  // FIXME: We still use the legacy pass manager to do code generation. We
  // create that pass manager here and use it as needed below.
  legacy::PassManager CodeGenPasses;
  bool NeedCodeGen = false;
  std::unique_ptr<llvm::ToolOutputFile> ThinLinkOS, DwoOS;

  // Append any output we need to the pass manager.
  switch (Action) {
  case Backend_EmitNothing:
    break;

  case Backend_EmitBC:
    if (CodeGenOpts.PrepareForThinLTO && !CodeGenOpts.DisableLLVMPasses) {
      if (!CodeGenOpts.ThinLinkBitcodeFile.empty()) {
        ThinLinkOS = openOutputFile(CodeGenOpts.ThinLinkBitcodeFile);
        if (!ThinLinkOS)
          return;
      }
      TheModule->addModuleFlag(Module::Error, "EnableSplitLTOUnit",
                               CodeGenOpts.EnableSplitLTOUnit);
      MPM.addPass(ThinLTOBitcodeWriterPass(*OS, ThinLinkOS ? &ThinLinkOS->os()
                                                           : nullptr));
    } else {
      // Emit a module summary by default for Regular LTO except for ld64
      // targets
      bool EmitLTOSummary =
          (CodeGenOpts.PrepareForLTO &&
           !CodeGenOpts.DisableLLVMPasses &&
           llvm::Triple(TheModule->getTargetTriple()).getVendor() !=
               llvm::Triple::Apple);
      if (EmitLTOSummary) {
        if (!TheModule->getModuleFlag("ThinLTO"))
          TheModule->addModuleFlag(Module::Error, "ThinLTO", uint32_t(0));
        TheModule->addModuleFlag(Module::Error, "EnableSplitLTOUnit",
                                 uint32_t(1));
      }
      MPM.addPass(
          BitcodeWriterPass(*OS, CodeGenOpts.EmitLLVMUseLists, EmitLTOSummary));
    }
    break;

  case Backend_EmitLL:
    MPM.addPass(PrintModulePass(*OS, "", CodeGenOpts.EmitLLVMUseLists));
    break;

  case Backend_EmitAssembly:
  case Backend_EmitMCNull:
  case Backend_EmitObj:
    NeedCodeGen = true;
    CodeGenPasses.add(
        createTargetTransformInfoWrapperPass(getTargetIRAnalysis()));
    if (!CodeGenOpts.SplitDwarfOutput.empty()) {
      DwoOS = openOutputFile(CodeGenOpts.SplitDwarfOutput);
      if (!DwoOS)
        return;
    }
    if (!AddEmitPasses(CodeGenPasses, Action, *OS,
                       DwoOS ? &DwoOS->os() : nullptr))
      // FIXME: Should we handle this error differently?
      return;
    break;
  }

  // Before executing passes, print the final values of the LLVM options.
  cl::PrintOptionValues();

  // Now that we have all of the passes ready, run them.
  {
    PrettyStackTraceString CrashInfo("Optimizer");
    MPM.run(*TheModule, MAM);
  }

  // Now if needed, run the legacy PM for codegen.
  if (NeedCodeGen) {
    PrettyStackTraceString CrashInfo("Code generation");
    CodeGenPasses.run(*TheModule);
  }

  if (ThinLinkOS)
    ThinLinkOS->keep();
  if (DwoOS)
    DwoOS->keep();
}

Expected<BitcodeModule> clang::FindThinLTOModule(MemoryBufferRef MBRef) {
  Expected<std::vector<BitcodeModule>> BMsOrErr = getBitcodeModuleList(MBRef);
  if (!BMsOrErr)
    return BMsOrErr.takeError();

  // The bitcode file may contain multiple modules, we want the one that is
  // marked as being the ThinLTO module.
  if (const BitcodeModule *Bm = FindThinLTOModule(*BMsOrErr))
    return *Bm;

  return make_error<StringError>("Could not find module summary",
                                 inconvertibleErrorCode());
}

BitcodeModule *clang::FindThinLTOModule(MutableArrayRef<BitcodeModule> BMs) {
  for (BitcodeModule &BM : BMs) {
    Expected<BitcodeLTOInfo> LTOInfo = BM.getLTOInfo();
    if (LTOInfo && LTOInfo->IsThinLTO)
      return &BM;
  }
  return nullptr;
}

static void runThinLTOBackend(
    DiagnosticsEngine &Diags, ModuleSummaryIndex *CombinedIndex, Module *M,
    const HeaderSearchOptions &HeaderOpts, const CodeGenOptions &CGOpts,
    const clang::TargetOptions &TOpts, const LangOptions &LOpts,
    std::unique_ptr<raw_pwrite_stream> OS, std::string SampleProfile,
    std::string ProfileRemapping, BackendAction Action) {
  StringMap<DenseMap<GlobalValue::GUID, GlobalValueSummary *>>
      ModuleToDefinedGVSummaries;
  CombinedIndex->collectDefinedGVSummariesPerModule(ModuleToDefinedGVSummaries);

  setCommandLineOpts(CGOpts);

  // We can simply import the values mentioned in the combined index, since
  // we should only invoke this using the individual indexes written out
  // via a WriteIndexesThinBackend.
  FunctionImporter::ImportMapTy ImportList;
  for (auto &GlobalList : *CombinedIndex) {
    // Ignore entries for undefined references.
    if (GlobalList.second.SummaryList.empty())
      continue;

    auto GUID = GlobalList.first;
    for (auto &Summary : GlobalList.second.SummaryList) {
      // Skip the summaries for the importing module. These are included to
      // e.g. record required linkage changes.
      if (Summary->modulePath() == M->getModuleIdentifier())
        continue;
      // Add an entry to provoke importing by thinBackend.
      ImportList[Summary->modulePath()].insert(GUID);
    }
  }

  std::vector<std::unique_ptr<llvm::MemoryBuffer>> OwnedImports;
  MapVector<llvm::StringRef, llvm::BitcodeModule> ModuleMap;

  for (auto &I : ImportList) {
    ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> MBOrErr =
        llvm::MemoryBuffer::getFile(I.first());
    if (!MBOrErr) {
      errs() << "Error loading imported file '" << I.first()
             << "': " << MBOrErr.getError().message() << "\n";
      return;
    }

    Expected<BitcodeModule> BMOrErr = FindThinLTOModule(**MBOrErr);
    if (!BMOrErr) {
      handleAllErrors(BMOrErr.takeError(), [&](ErrorInfoBase &EIB) {
        errs() << "Error loading imported file '" << I.first()
               << "': " << EIB.message() << '\n';
      });
      return;
    }
    ModuleMap.insert({I.first(), *BMOrErr});

    OwnedImports.push_back(std::move(*MBOrErr));
  }
  auto AddStream = [&](size_t Task) {
    return std::make_unique<lto::NativeObjectStream>(std::move(OS));
  };
  lto::Config Conf;
  if (CGOpts.SaveTempsFilePrefix != "") {
    if (Error E = Conf.addSaveTemps(CGOpts.SaveTempsFilePrefix + ".",
                                    /* UseInputModulePath */ false)) {
      handleAllErrors(std::move(E), [&](ErrorInfoBase &EIB) {
        errs() << "Error setting up ThinLTO save-temps: " << EIB.message()
               << '\n';
      });
    }
  }
  Conf.CPU = TOpts.CPU;
  Conf.CodeModel = getCodeModel(CGOpts);
  Conf.MAttrs = TOpts.Features;
  Conf.RelocModel = CGOpts.RelocationModel;
  Conf.CGOptLevel = getCGOptLevel(CGOpts);
  Conf.OptLevel = CGOpts.OptimizationLevel;
  initTargetOptions(Diags, Conf.Options, CGOpts, TOpts, LOpts, HeaderOpts);
  Conf.SampleProfile = std::move(SampleProfile);
  Conf.PTO.LoopUnrolling = CGOpts.UnrollLoops;
  // For historical reasons, loop interleaving is set to mirror setting for loop
  // unrolling.
  Conf.PTO.LoopInterleaving = CGOpts.UnrollLoops;
  Conf.PTO.LoopVectorization = CGOpts.VectorizeLoop;
  Conf.PTO.SLPVectorization = CGOpts.VectorizeSLP;
  Conf.PTO.CallGraphProfile = CGOpts.CallGraphProfile;

  // Context sensitive profile.
  if (CGOpts.hasProfileCSIRInstr()) {
    Conf.RunCSIRInstr = true;
    Conf.CSIRProfile = std::move(CGOpts.InstrProfileOutput);
  } else if (CGOpts.hasProfileCSIRUse()) {
    Conf.RunCSIRInstr = false;
    Conf.CSIRProfile = std::move(CGOpts.ProfileInstrumentUsePath);
  }

  Conf.ProfileRemapping = std::move(ProfileRemapping);
  Conf.UseNewPM = CGOpts.ExperimentalNewPassManager;
  Conf.DebugPassManager = CGOpts.DebugPassManager;
  Conf.RemarksWithHotness = CGOpts.DiagnosticsWithHotness;
  Conf.RemarksFilename = CGOpts.OptRecordFile;
  Conf.RemarksPasses = CGOpts.OptRecordPasses;
  Conf.RemarksFormat = CGOpts.OptRecordFormat;
  Conf.SplitDwarfFile = CGOpts.SplitDwarfFile;
  Conf.SplitDwarfOutput = CGOpts.SplitDwarfOutput;
  switch (Action) {
  case Backend_EmitNothing:
    Conf.PreCodeGenModuleHook = [](size_t Task, const Module &Mod) {
      return false;
    };
    break;
  case Backend_EmitLL:
    Conf.PreCodeGenModuleHook = [&](size_t Task, const Module &Mod) {
      M->print(*OS, nullptr, CGOpts.EmitLLVMUseLists);
      return false;
    };
    break;
  case Backend_EmitBC:
    Conf.PreCodeGenModuleHook = [&](size_t Task, const Module &Mod) {
      WriteBitcodeToFile(*M, *OS, CGOpts.EmitLLVMUseLists);
      return false;
    };
    break;
  default:
    Conf.CGFileType = getCodeGenFileType(Action);
    break;
  }
  if (Error E = thinBackend(
          Conf, -1, AddStream, *M, *CombinedIndex, ImportList,
          ModuleToDefinedGVSummaries[M->getModuleIdentifier()], ModuleMap)) {
    handleAllErrors(std::move(E), [&](ErrorInfoBase &EIB) {
      errs() << "Error running ThinLTO backend: " << EIB.message() << '\n';
    });
  }
}

void clang::EmitBackendOutput(DiagnosticsEngine &Diags,
                              const HeaderSearchOptions &HeaderOpts,
                              const CodeGenOptions &CGOpts,
                              const clang::TargetOptions &TOpts,
                              const LangOptions &LOpts,
                              const llvm::DataLayout &TDesc, Module *M,
                              BackendAction Action,
                              std::unique_ptr<raw_pwrite_stream> OS) {

  llvm::TimeTraceScope TimeScope("Backend");

  std::unique_ptr<llvm::Module> EmptyModule;
  if (!CGOpts.ThinLTOIndexFile.empty()) {
    // If we are performing a ThinLTO importing compile, load the function index
    // into memory and pass it into runThinLTOBackend, which will run the
    // function importer and invoke LTO passes.
    Expected<std::unique_ptr<ModuleSummaryIndex>> IndexOrErr =
        llvm::getModuleSummaryIndexForFile(CGOpts.ThinLTOIndexFile,
                                           /*IgnoreEmptyThinLTOIndexFile*/true);
    if (!IndexOrErr) {
      logAllUnhandledErrors(IndexOrErr.takeError(), errs(),
                            "Error loading index file '" +
                            CGOpts.ThinLTOIndexFile + "': ");
      return;
    }
    std::unique_ptr<ModuleSummaryIndex> CombinedIndex = std::move(*IndexOrErr);
    // A null CombinedIndex means we should skip ThinLTO compilation
    // (LLVM will optionally ignore empty index files, returning null instead
    // of an error).
    if (CombinedIndex) {
      if (!CombinedIndex->skipModuleByDistributedBackend()) {
        runThinLTOBackend(Diags, CombinedIndex.get(), M, HeaderOpts, CGOpts,
                          TOpts, LOpts, std::move(OS), CGOpts.SampleProfileFile,
                          CGOpts.ProfileRemappingFile, Action);
        return;
      }
      // Distributed indexing detected that nothing from the module is needed
      // for the final linking. So we can skip the compilation. We sill need to
      // output an empty object file to make sure that a linker does not fail
      // trying to read it. Also for some features, like CFI, we must skip
      // the compilation as CombinedIndex does not contain all required
      // information.
      EmptyModule = std::make_unique<llvm::Module>("empty", M->getContext());
      EmptyModule->setTargetTriple(M->getTargetTriple());
      M = EmptyModule.get();
    }
  }

  EmitAssemblyHelper AsmHelper(Diags, HeaderOpts, CGOpts, TOpts, LOpts, M);

  if (CGOpts.ExperimentalNewPassManager)
    AsmHelper.EmitAssemblyWithNewPassManager(Action, std::move(OS));
  else
    AsmHelper.EmitAssembly(Action, std::move(OS));

  // Verify clang's TargetInfo DataLayout against the LLVM TargetMachine's
  // DataLayout.
  if (AsmHelper.TM) {
    std::string DLDesc = M->getDataLayout().getStringRepresentation();
    if (DLDesc != TDesc.getStringRepresentation()) {
      unsigned DiagID = Diags.getCustomDiagID(
          DiagnosticsEngine::Error, "backend data layout '%0' does not match "
                                    "expected target description '%1'");
      Diags.Report(DiagID) << DLDesc << TDesc.getStringRepresentation();
    }
  }
}

// With -fembed-bitcode, save a copy of the llvm IR as data in the
// __LLVM,__bitcode section.
void clang::EmbedBitcode(llvm::Module *M, const CodeGenOptions &CGOpts,
                         llvm::MemoryBufferRef Buf) {
  if (CGOpts.getEmbedBitcode() == CodeGenOptions::Embed_Off)
    return;
  llvm::EmbedBitcodeInModule(
      *M, Buf, CGOpts.getEmbedBitcode() != CodeGenOptions::Embed_Marker,
      CGOpts.getEmbedBitcode() != CodeGenOptions::Embed_Bitcode,
      &CGOpts.CmdArgs);
}
