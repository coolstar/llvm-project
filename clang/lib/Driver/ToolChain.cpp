//===- ToolChain.cpp - Collections of tools for one platform --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/Driver/ToolChain.h"
#include "InputInfo.h"
#include "ToolChains/Arch/ARM.h"
#include "ToolChains/Clang.h"
#include "ToolChains/InterfaceStubs.h"
#include "ToolChains/Flang.h"
#include "clang/Basic/ObjCRuntime.h"
#include "clang/Basic/Sanitizers.h"
#include "clang/Config/config.h"
#include "clang/Driver/Action.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/DriverDiagnostic.h"
#include "clang/Driver/Job.h"
#include "clang/Driver/Options.h"
#include "clang/Driver/SanitizerArgs.h"
#include "clang/Driver/XRayArgs.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Triple.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/MC/MCTargetOptions.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/OptTable.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/TargetParser.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/VersionTuple.h"
#include "llvm/Support/VirtualFileSystem.h"
#include <cassert>
#include <cstddef>
#include <cstring>
#include <string>

using namespace clang;
using namespace driver;
using namespace tools;
using namespace llvm;
using namespace llvm::opt;

static llvm::opt::Arg *GetRTTIArgument(const ArgList &Args) {
  return Args.getLastArg(options::OPT_mkernel, options::OPT_fapple_kext,
                         options::OPT_fno_rtti, options::OPT_frtti);
}

static ToolChain::RTTIMode CalculateRTTIMode(const ArgList &Args,
                                             const llvm::Triple &Triple,
                                             const Arg *CachedRTTIArg) {
  // Explicit rtti/no-rtti args
  if (CachedRTTIArg) {
    if (CachedRTTIArg->getOption().matches(options::OPT_frtti))
      return ToolChain::RM_Enabled;
    else
      return ToolChain::RM_Disabled;
  }

  // -frtti is default, except for the PS4 CPU.
  return (Triple.isPS4CPU()) ? ToolChain::RM_Disabled : ToolChain::RM_Enabled;
}

ToolChain::ToolChain(const Driver &D, const llvm::Triple &T,
                     const ArgList &Args)
    : D(D), Triple(T), Args(Args), CachedRTTIArg(GetRTTIArgument(Args)),
      CachedRTTIMode(CalculateRTTIMode(Args, Triple, CachedRTTIArg)) {
  if (D.CCCIsCXX()) {
    if (auto CXXStdlibPath = getCXXStdlibPath())
      getFilePaths().push_back(*CXXStdlibPath);
  }

  if (auto RuntimePath = getRuntimePath())
    getLibraryPaths().push_back(*RuntimePath);

  std::string CandidateLibPath = getArchSpecificLibPath();
  if (getVFS().exists(CandidateLibPath))
    getFilePaths().push_back(CandidateLibPath);
}

void ToolChain::setTripleEnvironment(llvm::Triple::EnvironmentType Env) {
  Triple.setEnvironment(Env);
  if (EffectiveTriple != llvm::Triple())
    EffectiveTriple.setEnvironment(Env);
}

ToolChain::~ToolChain() = default;

llvm::vfs::FileSystem &ToolChain::getVFS() const {
  return getDriver().getVFS();
}

bool ToolChain::useIntegratedAs() const {
  return Args.hasFlag(options::OPT_fintegrated_as,
                      options::OPT_fno_integrated_as,
                      IsIntegratedAssemblerDefault());
}

bool ToolChain::useRelaxRelocations() const {
  return ENABLE_X86_RELAX_RELOCATIONS;
}

bool ToolChain::isNoExecStackDefault() const {
    return false;
}

const SanitizerArgs& ToolChain::getSanitizerArgs() const {
  if (!SanitizerArguments.get())
    SanitizerArguments.reset(new SanitizerArgs(*this, Args));
  return *SanitizerArguments.get();
}

const XRayArgs& ToolChain::getXRayArgs() const {
  if (!XRayArguments.get())
    XRayArguments.reset(new XRayArgs(*this, Args));
  return *XRayArguments.get();
}

namespace {

struct DriverSuffix {
  const char *Suffix;
  const char *ModeFlag;
};

} // namespace

static const DriverSuffix *FindDriverSuffix(StringRef ProgName, size_t &Pos) {
  // A list of known driver suffixes. Suffixes are compared against the
  // program name in order. If there is a match, the frontend type is updated as
  // necessary by applying the ModeFlag.
  static const DriverSuffix DriverSuffixes[] = {
      {"clang", nullptr},
      {"clang++", "--driver-mode=g++"},
      {"clang-c++", "--driver-mode=g++"},
      {"clang-cc", nullptr},
      {"clang-cpp", "--driver-mode=cpp"},
      {"clang-g++", "--driver-mode=g++"},
      {"clang-gcc", nullptr},
      {"clang-cl", "--driver-mode=cl"},
      {"cc", nullptr},
      {"cpp", "--driver-mode=cpp"},
      {"cl", "--driver-mode=cl"},
      {"++", "--driver-mode=g++"},
      {"flang", "--driver-mode=flang"},
  };

  for (size_t i = 0; i < llvm::array_lengthof(DriverSuffixes); ++i) {
    StringRef Suffix(DriverSuffixes[i].Suffix);
    if (ProgName.endswith(Suffix)) {
      Pos = ProgName.size() - Suffix.size();
      return &DriverSuffixes[i];
    }
  }
  return nullptr;
}

/// Normalize the program name from argv[0] by stripping the file extension if
/// present and lower-casing the string on Windows.
static std::string normalizeProgramName(llvm::StringRef Argv0) {
  std::string ProgName = std::string(llvm::sys::path::stem(Argv0));
#ifdef _WIN32
  // Transform to lowercase for case insensitive file systems.
  std::transform(ProgName.begin(), ProgName.end(), ProgName.begin(), ::tolower);
#endif
  return ProgName;
}

static const DriverSuffix *parseDriverSuffix(StringRef ProgName, size_t &Pos) {
  // Try to infer frontend type and default target from the program name by
  // comparing it against DriverSuffixes in order.

  // If there is a match, the function tries to identify a target as prefix.
  // E.g. "x86_64-linux-clang" as interpreted as suffix "clang" with target
  // prefix "x86_64-linux". If such a target prefix is found, it may be
  // added via -target as implicit first argument.
  const DriverSuffix *DS = FindDriverSuffix(ProgName, Pos);

  if (!DS) {
    // Try again after stripping any trailing version number:
    // clang++3.5 -> clang++
    ProgName = ProgName.rtrim("0123456789.");
    DS = FindDriverSuffix(ProgName, Pos);
  }

  if (!DS) {
    // Try again after stripping trailing -component.
    // clang++-tot -> clang++
    ProgName = ProgName.slice(0, ProgName.rfind('-'));
    DS = FindDriverSuffix(ProgName, Pos);
  }
  return DS;
}

ParsedClangName
ToolChain::getTargetAndModeFromProgramName(StringRef PN) {
  std::string ProgName = normalizeProgramName(PN);
  size_t SuffixPos;
  const DriverSuffix *DS = parseDriverSuffix(ProgName, SuffixPos);
  if (!DS)
    return {};
  size_t SuffixEnd = SuffixPos + strlen(DS->Suffix);

  size_t LastComponent = ProgName.rfind('-', SuffixPos);
  if (LastComponent == std::string::npos)
    return ParsedClangName(ProgName.substr(0, SuffixEnd), DS->ModeFlag);
  std::string ModeSuffix = ProgName.substr(LastComponent + 1,
                                           SuffixEnd - LastComponent - 1);

  // Infer target from the prefix.
  StringRef Prefix(ProgName);
  Prefix = Prefix.slice(0, LastComponent);
  std::string IgnoredError;
  bool IsRegistered =
      llvm::TargetRegistry::lookupTarget(std::string(Prefix), IgnoredError);
  return ParsedClangName{std::string(Prefix), ModeSuffix, DS->ModeFlag,
                         IsRegistered};
}

StringRef ToolChain::getDefaultUniversalArchName() const {
  // In universal driver terms, the arch name accepted by -arch isn't exactly
  // the same as the ones that appear in the triple. Roughly speaking, this is
  // an inverse of the darwin::getArchTypeForDarwinArchName() function.
  switch (Triple.getArch()) {
  case llvm::Triple::aarch64: {
    if (getTriple().getArchName() == "arm64e")
      return "arm64e";
    return "arm64";
  }
  case llvm::Triple::aarch64_32:
    return "arm64_32";
  case llvm::Triple::ppc:
    return "ppc";
  case llvm::Triple::ppc64:
    return "ppc64";
  case llvm::Triple::ppc64le:
    return "ppc64le";
  default:
    return Triple.getArchName();
  }
}

std::string ToolChain::getInputFilename(const InputInfo &Input) const {
  return Input.getFilename();
}

bool ToolChain::IsUnwindTablesDefault(const ArgList &Args) const {
  return false;
}

Tool *ToolChain::getClang() const {
  if (!Clang)
    Clang.reset(new tools::Clang(*this));
  return Clang.get();
}

Tool *ToolChain::getFlang() const {
  if (!Flang)
    Flang.reset(new tools::Flang(*this));
  return Flang.get();
}

Tool *ToolChain::buildAssembler() const {
  return new tools::ClangAs(*this);
}

Tool *ToolChain::buildLinker() const {
  llvm_unreachable("Linking is not supported by this toolchain");
}

Tool *ToolChain::getAssemble() const {
  if (!Assemble)
    Assemble.reset(buildAssembler());
  return Assemble.get();
}

Tool *ToolChain::getClangAs() const {
  if (!Assemble)
    Assemble.reset(new tools::ClangAs(*this));
  return Assemble.get();
}

Tool *ToolChain::getLink() const {
  if (!Link)
    Link.reset(buildLinker());
  return Link.get();
}

Tool *ToolChain::getIfsMerge() const {
  if (!IfsMerge)
    IfsMerge.reset(new tools::ifstool::Merger(*this));
  return IfsMerge.get();
}

Tool *ToolChain::getOffloadBundler() const {
  if (!OffloadBundler)
    OffloadBundler.reset(new tools::OffloadBundler(*this));
  return OffloadBundler.get();
}

Tool *ToolChain::getOffloadWrapper() const {
  if (!OffloadWrapper)
    OffloadWrapper.reset(new tools::OffloadWrapper(*this));
  return OffloadWrapper.get();
}

Tool *ToolChain::getTool(Action::ActionClass AC) const {
  switch (AC) {
  case Action::AssembleJobClass:
    return getAssemble();

  case Action::IfsMergeJobClass:
    return getIfsMerge();

  case Action::LinkJobClass:
    return getLink();

  case Action::InputClass:
  case Action::BindArchClass:
  case Action::OffloadClass:
  case Action::LipoJobClass:
  case Action::DsymutilJobClass:
  case Action::VerifyDebugInfoJobClass:
    llvm_unreachable("Invalid tool kind.");

  case Action::CompileJobClass:
  case Action::PrecompileJobClass:
  case Action::HeaderModulePrecompileJobClass:
  case Action::PreprocessJobClass:
  case Action::AnalyzeJobClass:
  case Action::MigrateJobClass:
  case Action::VerifyPCHJobClass:
  case Action::BackendJobClass:
    return getClang();

  case Action::OffloadBundlingJobClass:
  case Action::OffloadUnbundlingJobClass:
    return getOffloadBundler();

  case Action::OffloadWrapperJobClass:
    return getOffloadWrapper();
  }

  llvm_unreachable("Invalid tool kind.");
}

static StringRef getArchNameForCompilerRTLib(const ToolChain &TC,
                                             const ArgList &Args) {
  const llvm::Triple &Triple = TC.getTriple();
  bool IsWindows = Triple.isOSWindows();

  if (TC.getArch() == llvm::Triple::arm || TC.getArch() == llvm::Triple::armeb)
    return (arm::getARMFloatABI(TC, Args) == arm::FloatABI::Hard && !IsWindows)
               ? "armhf"
               : "arm";

  // For historic reasons, Android library is using i686 instead of i386.
  if (TC.getArch() == llvm::Triple::x86 && Triple.isAndroid())
    return "i686";

  return llvm::Triple::getArchTypeName(TC.getArch());
}

StringRef ToolChain::getOSLibName() const {
  switch (Triple.getOS()) {
  case llvm::Triple::FreeBSD:
    return "freebsd";
  case llvm::Triple::NetBSD:
    return "netbsd";
  case llvm::Triple::OpenBSD:
    return "openbsd";
  case llvm::Triple::Solaris:
    return "sunos";
  default:
    return getOS();
  }
}

std::string ToolChain::getCompilerRTPath() const {
  SmallString<128> Path(getDriver().ResourceDir);
  if (Triple.isOSUnknown()) {
    llvm::sys::path::append(Path, "lib");
  } else {
    llvm::sys::path::append(Path, "lib", getOSLibName());
  }
  return std::string(Path.str());
}

std::string ToolChain::getCompilerRTBasename(const ArgList &Args,
                                             StringRef Component, FileType Type,
                                             bool AddArch) const {
  const llvm::Triple &TT = getTriple();
  bool IsITANMSVCWindows =
      TT.isWindowsMSVCEnvironment() || TT.isWindowsItaniumEnvironment();

  const char *Prefix =
      IsITANMSVCWindows || Type == ToolChain::FT_Object ? "" : "lib";
  const char *Suffix;
  switch (Type) {
  case ToolChain::FT_Object:
    Suffix = IsITANMSVCWindows ? ".obj" : ".o";
    break;
  case ToolChain::FT_Static:
    Suffix = IsITANMSVCWindows ? ".lib" : ".a";
    break;
  case ToolChain::FT_Shared:
    Suffix = Triple.isOSWindows()
                 ? (Triple.isWindowsGNUEnvironment() ? ".dll.a" : ".lib")
                 : ".so";
    break;
  }

  std::string ArchAndEnv;
  if (AddArch) {
    StringRef Arch = getArchNameForCompilerRTLib(*this, Args);
    const char *Env = TT.isAndroid() ? "-android" : "";
    ArchAndEnv = ("-" + Arch + Env).str();
  }
  return (Prefix + Twine("clang_rt.") + Component + ArchAndEnv + Suffix).str();
}

std::string ToolChain::getCompilerRT(const ArgList &Args, StringRef Component,
                                     FileType Type) const {
  // Check for runtime files in the new layout without the architecture first.
  std::string CRTBasename =
      getCompilerRTBasename(Args, Component, Type, /*AddArch=*/false);
  for (const auto &LibPath : getLibraryPaths()) {
    SmallString<128> P(LibPath);
    llvm::sys::path::append(P, CRTBasename);
    if (getVFS().exists(P))
      return std::string(P.str());
  }

  // Fall back to the old expected compiler-rt name if the new one does not
  // exist.
  CRTBasename = getCompilerRTBasename(Args, Component, Type, /*AddArch=*/true);
  SmallString<128> Path(getCompilerRTPath());
  llvm::sys::path::append(Path, CRTBasename);
  return std::string(Path.str());
}

const char *ToolChain::getCompilerRTArgString(const llvm::opt::ArgList &Args,
                                              StringRef Component,
                                              FileType Type) const {
  return Args.MakeArgString(getCompilerRT(Args, Component, Type));
}


Optional<std::string> ToolChain::getRuntimePath() const {
  SmallString<128> P;

  // First try the triple passed to driver as --target=<triple>.
  P.assign(D.ResourceDir);
  llvm::sys::path::append(P, "lib", D.getTargetTriple());
  if (getVFS().exists(P))
    return llvm::Optional<std::string>(std::string(P.str()));

  // Second try the normalized triple.
  P.assign(D.ResourceDir);
  llvm::sys::path::append(P, "lib", Triple.str());
  if (getVFS().exists(P))
    return llvm::Optional<std::string>(std::string(P.str()));

  return None;
}

Optional<std::string> ToolChain::getCXXStdlibPath() const {
  SmallString<128> P;

  // First try the triple passed to driver as --target=<triple>.
  P.assign(D.Dir);
  llvm::sys::path::append(P, "..", "lib", D.getTargetTriple(), "c++");
  if (getVFS().exists(P))
    return llvm::Optional<std::string>(std::string(P.str()));

  // Second try the normalized triple.
  P.assign(D.Dir);
  llvm::sys::path::append(P, "..", "lib", Triple.str(), "c++");
  if (getVFS().exists(P))
    return llvm::Optional<std::string>(std::string(P.str()));

  return None;
}

std::string ToolChain::getArchSpecificLibPath() const {
  SmallString<128> Path(getDriver().ResourceDir);
  llvm::sys::path::append(Path, "lib", getOSLibName(),
                          llvm::Triple::getArchTypeName(getArch()));
  return std::string(Path.str());
}

bool ToolChain::needsProfileRT(const ArgList &Args) {
  if (Args.hasArg(options::OPT_noprofilelib))
    return false;

  return Args.hasArg(options::OPT_fprofile_generate) ||
         Args.hasArg(options::OPT_fprofile_generate_EQ) ||
         Args.hasArg(options::OPT_fcs_profile_generate) ||
         Args.hasArg(options::OPT_fcs_profile_generate_EQ) ||
         Args.hasArg(options::OPT_fprofile_instr_generate) ||
         Args.hasArg(options::OPT_fprofile_instr_generate_EQ) ||
         Args.hasArg(options::OPT_fcreate_profile) ||
         Args.hasArg(options::OPT_forder_file_instrumentation);
}

bool ToolChain::needsGCovInstrumentation(const llvm::opt::ArgList &Args) {
  return Args.hasArg(options::OPT_coverage) ||
         Args.hasFlag(options::OPT_fprofile_arcs, options::OPT_fno_profile_arcs,
                      false);
}

Tool *ToolChain::SelectTool(const JobAction &JA) const {
  if (D.IsFlangMode() && getDriver().ShouldUseFlangCompiler(JA)) return getFlang();
  if (getDriver().ShouldUseClangCompiler(JA)) return getClang();
  Action::ActionClass AC = JA.getKind();
  if (AC == Action::AssembleJobClass && useIntegratedAs())
    return getClangAs();
  return getTool(AC);
}

std::string ToolChain::GetFilePath(const char *Name) const {
  return D.GetFilePath(Name, *this);
}

std::string ToolChain::GetProgramPath(const char *Name) const {
  return D.GetProgramPath(Name, *this);
}

std::string ToolChain::GetLinkerPath() const {
  const Arg* A = Args.getLastArg(options::OPT_fuse_ld_EQ);
  StringRef UseLinker = A ? A->getValue() : CLANG_DEFAULT_LINKER;

  if (llvm::sys::path::is_absolute(UseLinker)) {
    // If we're passed what looks like an absolute path, don't attempt to
    // second-guess that.
    if (llvm::sys::fs::can_execute(UseLinker))
      return std::string(UseLinker);
  } else if (UseLinker.empty() || UseLinker == "ld") {
    // If we're passed -fuse-ld= with no argument, or with the argument ld,
    // then use whatever the default system linker is.
    return GetProgramPath(getDefaultLinker());
  } else {
    llvm::SmallString<8> LinkerName;
    if (Triple.isOSDarwin())
      LinkerName.append("ld64.");
    else
      LinkerName.append("ld.");
    LinkerName.append(UseLinker);

    std::string LinkerPath(GetProgramPath(LinkerName.c_str()));
    if (llvm::sys::fs::can_execute(LinkerPath))
      return LinkerPath;
  }

  if (A)
    getDriver().Diag(diag::err_drv_invalid_linker_name) << A->getAsString(Args);

  return GetProgramPath(getDefaultLinker());
}

types::ID ToolChain::LookupTypeForExtension(StringRef Ext) const {
  types::ID id = types::lookupTypeForExtension(Ext);

  // Flang always runs the preprocessor and has no notion of "preprocessed
  // fortran". Here, TY_PP_Fortran is coerced to TY_Fortran to avoid treating
  // them differently.
  if (D.IsFlangMode() && id == types::TY_PP_Fortran)
    id = types::TY_Fortran;

  return id;
}

bool ToolChain::HasNativeLLVMSupport() const {
  return false;
}

bool ToolChain::isCrossCompiling() const {
  llvm::Triple HostTriple(LLVM_HOST_TRIPLE);
  switch (HostTriple.getArch()) {
  // The A32/T32/T16 instruction sets are not separate architectures in this
  // context.
  case llvm::Triple::arm:
  case llvm::Triple::armeb:
  case llvm::Triple::thumb:
  case llvm::Triple::thumbeb:
    return getArch() != llvm::Triple::arm && getArch() != llvm::Triple::thumb &&
           getArch() != llvm::Triple::armeb && getArch() != llvm::Triple::thumbeb;
  default:
    return HostTriple.getArch() != getArch();
  }
}

ObjCRuntime ToolChain::getDefaultObjCRuntime(bool isNonFragile) const {
  return ObjCRuntime(isNonFragile ? ObjCRuntime::GNUstep : ObjCRuntime::GCC,
                     VersionTuple());
}

llvm::ExceptionHandling
ToolChain::GetExceptionModel(const llvm::opt::ArgList &Args) const {
  return llvm::ExceptionHandling::None;
}

bool ToolChain::isThreadModelSupported(const StringRef Model) const {
  if (Model == "single") {
    // FIXME: 'single' is only supported on ARM and WebAssembly so far.
    return Triple.getArch() == llvm::Triple::arm ||
           Triple.getArch() == llvm::Triple::armeb ||
           Triple.getArch() == llvm::Triple::thumb ||
           Triple.getArch() == llvm::Triple::thumbeb ||
           Triple.getArch() == llvm::Triple::wasm32 ||
           Triple.getArch() == llvm::Triple::wasm64;
  } else if (Model == "posix")
    return true;

  return false;
}

std::string ToolChain::ComputeLLVMTriple(const ArgList &Args,
                                         types::ID InputType) const {
  switch (getTriple().getArch()) {
  default:
    return getTripleString();

  case llvm::Triple::x86_64: {
    llvm::Triple Triple = getTriple();
    if (!Triple.isOSBinFormatMachO())
      return getTripleString();

    if (Arg *A = Args.getLastArg(options::OPT_march_EQ)) {
      // x86_64h goes in the triple. Other -march options just use the
      // vanilla triple we already have.
      StringRef MArch = A->getValue();
      if (MArch == "x86_64h")
        Triple.setArchName(MArch);
    }
    return Triple.getTriple();
  }
  case llvm::Triple::aarch64: {
    llvm::Triple Triple = getTriple();
    if (!Triple.isOSBinFormatMachO())
      return getTripleString();

    StringRef Arch = Triple.getArchName();
    if (Arch == "arm64e")
      return Triple.getTriple();

    // FIXME: older versions of ld64 expect the "arm64" component in the actual
    // triple string and query it to determine whether an LTO file can be
    // handled. Remove this when we don't care any more.
    Triple.setArchName("arm64");
    return Triple.getTriple();
  }
  case llvm::Triple::aarch64_32:
    return getTripleString();
  case llvm::Triple::arm:
  case llvm::Triple::armeb:
  case llvm::Triple::thumb:
  case llvm::Triple::thumbeb: {
    // FIXME: Factor into subclasses.
    llvm::Triple Triple = getTriple();
    bool IsBigEndian = getTriple().getArch() == llvm::Triple::armeb ||
                       getTriple().getArch() == llvm::Triple::thumbeb;

    // Handle pseudo-target flags '-mlittle-endian'/'-EL' and
    // '-mbig-endian'/'-EB'.
    if (Arg *A = Args.getLastArg(options::OPT_mlittle_endian,
                                 options::OPT_mbig_endian)) {
      IsBigEndian = !A->getOption().matches(options::OPT_mlittle_endian);
    }

    // Thumb2 is the default for V7 on Darwin.
    //
    // FIXME: Thumb should just be another -target-feaure, not in the triple.
    StringRef MCPU, MArch;
    if (const Arg *A = Args.getLastArg(options::OPT_mcpu_EQ))
      MCPU = A->getValue();
    if (const Arg *A = Args.getLastArg(options::OPT_march_EQ))
      MArch = A->getValue();
    std::string CPU =
        Triple.isOSBinFormatMachO()
            ? tools::arm::getARMCPUForMArch(MArch, Triple).str()
            : tools::arm::getARMTargetCPU(MCPU, MArch, Triple);
    StringRef Suffix =
      tools::arm::getLLVMArchSuffixForARM(CPU, MArch, Triple);
    bool IsMProfile = ARM::parseArchProfile(Suffix) == ARM::ProfileKind::M;
    bool ThumbDefault = IsMProfile || (ARM::parseArchVersion(Suffix) == 7 &&
                                       getTriple().isOSBinFormatMachO());
    // FIXME: this is invalid for WindowsCE
    if (getTriple().isOSWindows())
      ThumbDefault = true;
    std::string ArchName;
    if (IsBigEndian)
      ArchName = "armeb";
    else
      ArchName = "arm";

    // Check if ARM ISA was explicitly selected (using -mno-thumb or -marm) for
    // M-Class CPUs/architecture variants, which is not supported.
    bool ARMModeRequested = !Args.hasFlag(options::OPT_mthumb,
                                          options::OPT_mno_thumb, ThumbDefault);
    if (IsMProfile && ARMModeRequested) {
      if (!MCPU.empty())
        getDriver().Diag(diag::err_cpu_unsupported_isa) << CPU << "ARM";
       else
        getDriver().Diag(diag::err_arch_unsupported_isa)
          << tools::arm::getARMArch(MArch, getTriple()) << "ARM";
    }

    // Check to see if an explicit choice to use thumb has been made via
    // -mthumb. For assembler files we must check for -mthumb in the options
    // passed to the assembler via -Wa or -Xassembler.
    bool IsThumb = false;
    if (InputType != types::TY_PP_Asm)
      IsThumb = Args.hasFlag(options::OPT_mthumb, options::OPT_mno_thumb,
                              ThumbDefault);
    else {
      // Ideally we would check for these flags in
      // CollectArgsForIntegratedAssembler but we can't change the ArchName at
      // that point. There is no assembler equivalent of -mno-thumb, -marm, or
      // -mno-arm.
      for (const auto *A :
           Args.filtered(options::OPT_Wa_COMMA, options::OPT_Xassembler)) {
        for (StringRef Value : A->getValues()) {
          if (Value == "-mthumb")
            IsThumb = true;
        }
      }
    }
    // Assembly files should start in ARM mode, unless arch is M-profile, or
    // -mthumb has been passed explicitly to the assembler. Windows is always
    // thumb.
    if (IsThumb || IsMProfile || getTriple().isOSWindows()) {
      if (IsBigEndian)
        ArchName = "thumbeb";
      else
        ArchName = "thumb";
    }
    Triple.setArchName(ArchName + Suffix.str());

    return Triple.getTriple();
  }
  }
}

std::string ToolChain::ComputeEffectiveClangTriple(const ArgList &Args,
                                                   types::ID InputType) const {
  return ComputeLLVMTriple(Args, InputType);
}

void ToolChain::AddClangSystemIncludeArgs(const ArgList &DriverArgs,
                                          ArgStringList &CC1Args) const {
  // Each toolchain should provide the appropriate include flags.
}

void ToolChain::addClangTargetOptions(
    const ArgList &DriverArgs, ArgStringList &CC1Args,
    Action::OffloadKind DeviceOffloadKind) const {}

void ToolChain::addClangWarningOptions(ArgStringList &CC1Args) const {}

void ToolChain::addProfileRTLibs(const llvm::opt::ArgList &Args,
                                 llvm::opt::ArgStringList &CmdArgs) const {
  if (!needsProfileRT(Args) && !needsGCovInstrumentation(Args))
    return;

  CmdArgs.push_back(getCompilerRTArgString(Args, "profile"));
}

ToolChain::RuntimeLibType ToolChain::GetRuntimeLibType(
    const ArgList &Args) const {
  const Arg* A = Args.getLastArg(options::OPT_rtlib_EQ);
  StringRef LibName = A ? A->getValue() : CLANG_DEFAULT_RTLIB;

  // Only use "platform" in tests to override CLANG_DEFAULT_RTLIB!
  if (LibName == "compiler-rt")
    return ToolChain::RLT_CompilerRT;
  else if (LibName == "libgcc")
    return ToolChain::RLT_Libgcc;
  else if (LibName == "platform")
    return GetDefaultRuntimeLibType();

  if (A)
    getDriver().Diag(diag::err_drv_invalid_rtlib_name) << A->getAsString(Args);

  return GetDefaultRuntimeLibType();
}

ToolChain::UnwindLibType ToolChain::GetUnwindLibType(
    const ArgList &Args) const {
  const Arg *A = Args.getLastArg(options::OPT_unwindlib_EQ);
  StringRef LibName = A ? A->getValue() : CLANG_DEFAULT_UNWINDLIB;

  if (LibName == "none")
    return ToolChain::UNW_None;
  else if (LibName == "platform" || LibName == "") {
    ToolChain::RuntimeLibType RtLibType = GetRuntimeLibType(Args);
    if (RtLibType == ToolChain::RLT_CompilerRT)
      return ToolChain::UNW_None;
    else if (RtLibType == ToolChain::RLT_Libgcc)
      return ToolChain::UNW_Libgcc;
  } else if (LibName == "libunwind") {
    if (GetRuntimeLibType(Args) == RLT_Libgcc)
      getDriver().Diag(diag::err_drv_incompatible_unwindlib);
    return ToolChain::UNW_CompilerRT;
  } else if (LibName == "libgcc")
    return ToolChain::UNW_Libgcc;

  if (A)
    getDriver().Diag(diag::err_drv_invalid_unwindlib_name)
        << A->getAsString(Args);

  return GetDefaultUnwindLibType();
}

ToolChain::CXXStdlibType ToolChain::GetCXXStdlibType(const ArgList &Args) const{
  const Arg *A = Args.getLastArg(options::OPT_stdlib_EQ);
  StringRef LibName = A ? A->getValue() : CLANG_DEFAULT_CXX_STDLIB;

  // Only use "platform" in tests to override CLANG_DEFAULT_CXX_STDLIB!
  if (LibName == "libc++")
    return ToolChain::CST_Libcxx;
  else if (LibName == "libstdc++")
    return ToolChain::CST_Libstdcxx;
  else if (LibName == "platform")
    return GetDefaultCXXStdlibType();

  if (A)
    getDriver().Diag(diag::err_drv_invalid_stdlib_name) << A->getAsString(Args);

  return GetDefaultCXXStdlibType();
}

/// Utility function to add a system include directory to CC1 arguments.
/*static*/ void ToolChain::addSystemInclude(const ArgList &DriverArgs,
                                            ArgStringList &CC1Args,
                                            const Twine &Path) {
  CC1Args.push_back("-internal-isystem");
  CC1Args.push_back(DriverArgs.MakeArgString(Path));
}

/// Utility function to add a system include directory with extern "C"
/// semantics to CC1 arguments.
///
/// Note that this should be used rarely, and only for directories that
/// historically and for legacy reasons are treated as having implicit extern
/// "C" semantics. These semantics are *ignored* by and large today, but its
/// important to preserve the preprocessor changes resulting from the
/// classification.
/*static*/ void ToolChain::addExternCSystemInclude(const ArgList &DriverArgs,
                                                   ArgStringList &CC1Args,
                                                   const Twine &Path) {
  CC1Args.push_back("-internal-externc-isystem");
  CC1Args.push_back(DriverArgs.MakeArgString(Path));
}

void ToolChain::addExternCSystemIncludeIfExists(const ArgList &DriverArgs,
                                                ArgStringList &CC1Args,
                                                const Twine &Path) {
  if (llvm::sys::fs::exists(Path))
    addExternCSystemInclude(DriverArgs, CC1Args, Path);
}

/// Utility function to add a list of system include directories to CC1.
/*static*/ void ToolChain::addSystemIncludes(const ArgList &DriverArgs,
                                             ArgStringList &CC1Args,
                                             ArrayRef<StringRef> Paths) {
  for (const auto &Path : Paths) {
    CC1Args.push_back("-internal-isystem");
    CC1Args.push_back(DriverArgs.MakeArgString(Path));
  }
}

void ToolChain::AddClangCXXStdlibIncludeArgs(const ArgList &DriverArgs,
                                             ArgStringList &CC1Args) const {
  // Header search paths should be handled by each of the subclasses.
  // Historically, they have not been, and instead have been handled inside of
  // the CC1-layer frontend. As the logic is hoisted out, this generic function
  // will slowly stop being called.
  //
  // While it is being called, replicate a bit of a hack to propagate the
  // '-stdlib=' flag down to CC1 so that it can in turn customize the C++
  // header search paths with it. Once all systems are overriding this
  // function, the CC1 flag and this line can be removed.
  DriverArgs.AddAllArgs(CC1Args, options::OPT_stdlib_EQ);
}

void ToolChain::AddClangCXXStdlibIsystemArgs(
    const llvm::opt::ArgList &DriverArgs,
    llvm::opt::ArgStringList &CC1Args) const {
  DriverArgs.ClaimAllArgs(options::OPT_stdlibxx_isystem);
  if (!DriverArgs.hasArg(options::OPT_nostdincxx))
    for (const auto &P :
         DriverArgs.getAllArgValues(options::OPT_stdlibxx_isystem))
      addSystemInclude(DriverArgs, CC1Args, P);
}

bool ToolChain::ShouldLinkCXXStdlib(const llvm::opt::ArgList &Args) const {
  return getDriver().CCCIsCXX() &&
         !Args.hasArg(options::OPT_nostdlib, options::OPT_nodefaultlibs,
                      options::OPT_nostdlibxx);
}

void ToolChain::AddCXXStdlibLibArgs(const ArgList &Args,
                                    ArgStringList &CmdArgs) const {
  assert(!Args.hasArg(options::OPT_nostdlibxx) &&
         "should not have called this");
  CXXStdlibType Type = GetCXXStdlibType(Args);

  switch (Type) {
  case ToolChain::CST_Libcxx:
    CmdArgs.push_back("-lc++");
    break;

  case ToolChain::CST_Libstdcxx:
    CmdArgs.push_back("-lstdc++");
    break;
  }
}

void ToolChain::AddFilePathLibArgs(const ArgList &Args,
                                   ArgStringList &CmdArgs) const {
  for (const auto &LibPath : getFilePaths())
    if(LibPath.length() > 0)
      CmdArgs.push_back(Args.MakeArgString(StringRef("-L") + LibPath));
}

void ToolChain::AddCCKextLibArgs(const ArgList &Args,
                                 ArgStringList &CmdArgs) const {
  CmdArgs.push_back("-lcc_kext");
}

bool ToolChain::isFastMathRuntimeAvailable(const ArgList &Args,
                                           std::string &Path) const {
  // Do not check for -fno-fast-math or -fno-unsafe-math when -Ofast passed
  // (to keep the linker options consistent with gcc and clang itself).
  if (!isOptimizationLevelFast(Args)) {
    // Check if -ffast-math or -funsafe-math.
    Arg *A =
      Args.getLastArg(options::OPT_ffast_math, options::OPT_fno_fast_math,
                      options::OPT_funsafe_math_optimizations,
                      options::OPT_fno_unsafe_math_optimizations);

    if (!A || A->getOption().getID() == options::OPT_fno_fast_math ||
        A->getOption().getID() == options::OPT_fno_unsafe_math_optimizations)
      return false;
  }
  // If crtfastmath.o exists add it to the arguments.
  Path = GetFilePath("crtfastmath.o");
  return (Path != "crtfastmath.o"); // Not found.
}

bool ToolChain::addFastMathRuntimeIfAvailable(const ArgList &Args,
                                              ArgStringList &CmdArgs) const {
  std::string Path;
  if (isFastMathRuntimeAvailable(Args, Path)) {
    CmdArgs.push_back(Args.MakeArgString(Path));
    return true;
  }

  return false;
}

SanitizerMask ToolChain::getSupportedSanitizers() const {
  // Return sanitizers which don't require runtime support and are not
  // platform dependent.

  SanitizerMask Res = (SanitizerKind::Undefined & ~SanitizerKind::Vptr &
                       ~SanitizerKind::Function) |
                      (SanitizerKind::CFI & ~SanitizerKind::CFIICall) |
                      SanitizerKind::CFICastStrict |
                      SanitizerKind::FloatDivideByZero |
                      SanitizerKind::UnsignedIntegerOverflow |
                      SanitizerKind::ImplicitConversion |
                      SanitizerKind::Nullability | SanitizerKind::LocalBounds;
  if (getTriple().getArch() == llvm::Triple::x86 ||
      getTriple().getArch() == llvm::Triple::x86_64 ||
      getTriple().getArch() == llvm::Triple::arm ||
      getTriple().getArch() == llvm::Triple::wasm32 ||
      getTriple().getArch() == llvm::Triple::wasm64 || getTriple().isAArch64())
    Res |= SanitizerKind::CFIICall;
  if (getTriple().getArch() == llvm::Triple::x86_64 || getTriple().isAArch64())
    Res |= SanitizerKind::ShadowCallStack;
  if (getTriple().isAArch64())
    Res |= SanitizerKind::MemTag;
  return Res;
}

void ToolChain::AddCudaIncludeArgs(const ArgList &DriverArgs,
                                   ArgStringList &CC1Args) const {}

void ToolChain::AddIAMCUIncludeArgs(const ArgList &DriverArgs,
                                    ArgStringList &CC1Args) const {}

static VersionTuple separateMSVCFullVersion(unsigned Version) {
  if (Version < 100)
    return VersionTuple(Version);

  if (Version < 10000)
    return VersionTuple(Version / 100, Version % 100);

  unsigned Build = 0, Factor = 1;
  for (; Version > 10000; Version = Version / 10, Factor = Factor * 10)
    Build = Build + (Version % 10) * Factor;
  return VersionTuple(Version / 100, Version % 100, Build);
}

VersionTuple
ToolChain::computeMSVCVersion(const Driver *D,
                              const llvm::opt::ArgList &Args) const {
  const Arg *MSCVersion = Args.getLastArg(options::OPT_fmsc_version);
  const Arg *MSCompatibilityVersion =
      Args.getLastArg(options::OPT_fms_compatibility_version);

  if (MSCVersion && MSCompatibilityVersion) {
    if (D)
      D->Diag(diag::err_drv_argument_not_allowed_with)
          << MSCVersion->getAsString(Args)
          << MSCompatibilityVersion->getAsString(Args);
    return VersionTuple();
  }

  if (MSCompatibilityVersion) {
    VersionTuple MSVT;
    if (MSVT.tryParse(MSCompatibilityVersion->getValue())) {
      if (D)
        D->Diag(diag::err_drv_invalid_value)
            << MSCompatibilityVersion->getAsString(Args)
            << MSCompatibilityVersion->getValue();
    } else {
      return MSVT;
    }
  }

  if (MSCVersion) {
    unsigned Version = 0;
    if (StringRef(MSCVersion->getValue()).getAsInteger(10, Version)) {
      if (D)
        D->Diag(diag::err_drv_invalid_value)
            << MSCVersion->getAsString(Args) << MSCVersion->getValue();
    } else {
      return separateMSVCFullVersion(Version);
    }
  }

  return VersionTuple();
}

llvm::opt::DerivedArgList *ToolChain::TranslateOpenMPTargetArgs(
    const llvm::opt::DerivedArgList &Args, bool SameTripleAsHost,
    SmallVectorImpl<llvm::opt::Arg *> &AllocatedArgs) const {
  DerivedArgList *DAL = new DerivedArgList(Args.getBaseArgs());
  const OptTable &Opts = getDriver().getOpts();
  bool Modified = false;

  // Handle -Xopenmp-target flags
  for (auto *A : Args) {
    // Exclude flags which may only apply to the host toolchain.
    // Do not exclude flags when the host triple (AuxTriple)
    // matches the current toolchain triple. If it is not present
    // at all, target and host share a toolchain.
    if (A->getOption().matches(options::OPT_m_Group)) {
      if (SameTripleAsHost)
        DAL->append(A);
      else
        Modified = true;
      continue;
    }

    unsigned Index;
    unsigned Prev;
    bool XOpenMPTargetNoTriple =
        A->getOption().matches(options::OPT_Xopenmp_target);

    if (A->getOption().matches(options::OPT_Xopenmp_target_EQ)) {
      // Passing device args: -Xopenmp-target=<triple> -opt=val.
      if (A->getValue(0) == getTripleString())
        Index = Args.getBaseArgs().MakeIndex(A->getValue(1));
      else
        continue;
    } else if (XOpenMPTargetNoTriple) {
      // Passing device args: -Xopenmp-target -opt=val.
      Index = Args.getBaseArgs().MakeIndex(A->getValue(0));
    } else {
      DAL->append(A);
      continue;
    }

    // Parse the argument to -Xopenmp-target.
    Prev = Index;
    std::unique_ptr<Arg> XOpenMPTargetArg(Opts.ParseOneArg(Args, Index));
    if (!XOpenMPTargetArg || Index > Prev + 1) {
      getDriver().Diag(diag::err_drv_invalid_Xopenmp_target_with_args)
          << A->getAsString(Args);
      continue;
    }
    if (XOpenMPTargetNoTriple && XOpenMPTargetArg &&
        Args.getAllArgValues(options::OPT_fopenmp_targets_EQ).size() != 1) {
      getDriver().Diag(diag::err_drv_Xopenmp_target_missing_triple);
      continue;
    }
    XOpenMPTargetArg->setBaseArg(A);
    A = XOpenMPTargetArg.release();
    AllocatedArgs.push_back(A);
    DAL->append(A);
    Modified = true;
  }

  if (Modified)
    return DAL;

  delete DAL;
  return nullptr;
}

// TODO: Currently argument values separated by space e.g.
// -Xclang -mframe-pointer=no cannot be passed by -Xarch_. This should be
// fixed.
void ToolChain::TranslateXarchArgs(
    const llvm::opt::DerivedArgList &Args, llvm::opt::Arg *&A,
    llvm::opt::DerivedArgList *DAL,
    SmallVectorImpl<llvm::opt::Arg *> *AllocatedArgs) const {
  const OptTable &Opts = getDriver().getOpts();
  unsigned ValuePos = 1;
  if (A->getOption().matches(options::OPT_Xarch_device) ||
      A->getOption().matches(options::OPT_Xarch_host))
    ValuePos = 0;

  unsigned Index = Args.getBaseArgs().MakeIndex(A->getValue(ValuePos));
  unsigned Prev = Index;
  std::unique_ptr<llvm::opt::Arg> XarchArg(Opts.ParseOneArg(Args, Index));

  // If the argument parsing failed or more than one argument was
  // consumed, the -Xarch_ argument's parameter tried to consume
  // extra arguments. Emit an error and ignore.
  //
  // We also want to disallow any options which would alter the
  // driver behavior; that isn't going to work in our model. We
  // use isDriverOption() as an approximation, although things
  // like -O4 are going to slip through.
  if (!XarchArg || Index > Prev + 1) {
    getDriver().Diag(diag::err_drv_invalid_Xarch_argument_with_args)
        << A->getAsString(Args);
    return;
  } else if (XarchArg->getOption().hasFlag(options::DriverOption)) {
    getDriver().Diag(diag::err_drv_invalid_Xarch_argument_isdriver)
        << A->getAsString(Args);
    return;
  }
  XarchArg->setBaseArg(A);
  A = XarchArg.release();
  if (!AllocatedArgs)
    DAL->AddSynthesizedArg(A);
  else
    AllocatedArgs->push_back(A);
}

llvm::opt::DerivedArgList *ToolChain::TranslateXarchArgs(
    const llvm::opt::DerivedArgList &Args, StringRef BoundArch,
    Action::OffloadKind OFK,
    SmallVectorImpl<llvm::opt::Arg *> *AllocatedArgs) const {
  DerivedArgList *DAL = new DerivedArgList(Args.getBaseArgs());
  bool Modified = false;

  bool IsGPU = OFK == Action::OFK_Cuda || OFK == Action::OFK_HIP;
  for (Arg *A : Args) {
    bool NeedTrans = false;
    bool Skip = false;
    if (A->getOption().matches(options::OPT_Xarch_device)) {
      NeedTrans = IsGPU;
      Skip = !IsGPU;
    } else if (A->getOption().matches(options::OPT_Xarch_host)) {
      NeedTrans = !IsGPU;
      Skip = IsGPU;
    } else if (A->getOption().matches(options::OPT_Xarch__) && IsGPU) {
      // Do not translate -Xarch_ options for non CUDA/HIP toolchain since
      // they may need special translation.
      // Skip this argument unless the architecture matches BoundArch
      if (BoundArch.empty() || A->getValue(0) != BoundArch)
        Skip = true;
      else
        NeedTrans = true;
    }
    if (NeedTrans || Skip)
      Modified = true;
    if (NeedTrans)
      TranslateXarchArgs(Args, A, DAL, AllocatedArgs);
    if (!Skip)
      DAL->append(A);
  }

  if (Modified)
    return DAL;

  delete DAL;
  return nullptr;
}
