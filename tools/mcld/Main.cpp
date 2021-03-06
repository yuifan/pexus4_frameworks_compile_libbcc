/*
 * Copyright 2012, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdlib.h>
#include <string>

#include <llvm/ADT/SmallString.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/system_error.h>

#include <mcld/Config/Config.h>

#include <bcc/Config/Config.h>
#include <bcc/Support/LinkerConfig.h>
#include <bcc/Support/Initialization.h>
#include <bcc/Support/TargetLinkerConfigs.h>
#include <bcc/Linker.h>

using namespace bcc;

//===----------------------------------------------------------------------===//
// Compiler Options
//===----------------------------------------------------------------------===//
#ifdef TARGET_BUILD
static const std::string OptTargetTripe(DEFAULT_TARGET_TRIPLE_STRING);
#else
static llvm::cl::opt<std::string>
OptTargetTriple("mtriple",
                llvm::cl::desc("Specify the target triple (default: "
                                DEFAULT_TARGET_TRIPLE_STRING ")"),
                llvm::cl::init(DEFAULT_TARGET_TRIPLE_STRING),
                llvm::cl::value_desc("triple"));

static llvm::cl::alias OptTargetTripleC("C", llvm::cl::NotHidden,
                                        llvm::cl::desc("Alias for -mtriple"),
                                        llvm::cl::aliasopt(OptTargetTriple));
#endif

//===----------------------------------------------------------------------===//
// Command Line Options
// There are four kinds of command line options:
//   1. input, (may be a file, such as -m and /tmp/XXXX.o.)
//   2. scripting options, (represent a subset of link scripting language, such
//      as --defsym.)
//   3. and general options. (the rest of options)
//===----------------------------------------------------------------------===//
// General Options
//===----------------------------------------------------------------------===//
static llvm::cl::opt<std::string>
OptOutputFilename("o",
                  llvm::cl::desc("Output filename"),
                  llvm::cl::value_desc("filename"));

static llvm::cl::opt<std::string>
OptSysRoot("sysroot", llvm::cl::desc("Use directory as the location of the "
                                     "sysroot, overriding the configure-time "
                                     "default."),
           llvm::cl::value_desc("directory"),
           llvm::cl::ValueRequired);

static llvm::cl::list<std::string>
OptSearchDirList("L",
                 llvm::cl::ZeroOrMore,
                 llvm::cl::desc("Add path searchdir to the list of paths that "
                                "mcld will search for archive libraries and "
                                "mcld control scripts."),
                 llvm::cl::value_desc("searchdir"),
                 llvm::cl::Prefix);

static llvm::cl::opt<std::string>
OptSOName("soname",
          llvm::cl::desc("Set internal name of shared library"),
          llvm::cl::value_desc("name"));


static llvm::cl::opt<bool>
OptShared("shared",
          llvm::cl::desc("Create a shared library."),
          llvm::cl::init(false));

static llvm::cl::opt<bool>
OptBsymbolic("Bsymbolic",
             llvm::cl::desc("Bind references within the shared library."),
             llvm::cl::init(true));

static llvm::cl::opt<std::string>
OptDyld("dynamic-linker",
        llvm::cl::desc("Set the name of the dynamic linker."),
        llvm::cl::value_desc("Program"));

//===----------------------------------------------------------------------===//
// Inputs
//===----------------------------------------------------------------------===//
static llvm::cl::list<std::string>
OptInputObjectFiles(llvm::cl::Positional,
                    llvm::cl::desc("[input object files]"),
                    llvm::cl::OneOrMore);

static llvm::cl::list<std::string>
OptNameSpecList("l",
                llvm::cl::ZeroOrMore,
                llvm::cl::desc("Add the archive or object file specified by "
                               "namespec to the list of files to link."),
                llvm::cl::value_desc("namespec"),
                llvm::cl::Prefix);

//===----------------------------------------------------------------------===//
// Scripting Options
//===----------------------------------------------------------------------===//
static llvm::cl::list<std::string>
OptWrapList("wrap",
            llvm::cl::ZeroOrMore,
            llvm::cl::desc("Use a wrap function fo symbol."),
            llvm::cl::value_desc("symbol"));

static llvm::cl::list<std::string>
OptPortableList("portable",
                llvm::cl::ZeroOrMore,
                llvm::cl::desc("Use a portable function to symbol."),
                llvm::cl::value_desc("symbol"));

//===----------------------------------------------------------------------===//
// Helper Functions
//===----------------------------------------------------------------------===//
// Override "mcld -version"
static void MCLDVersionPrinter() {
  llvm::raw_ostream &os = llvm::outs();
  os << "mcld (The MCLinker Project, http://mclinker.googlecode.com/):\n"
     << "  version: " MCLD_VERSION "\n"
     << "  Default target: " << DEFAULT_TARGET_TRIPLE_STRING << "\n";

  os << "\n";

  os << "LLVM (http://llvm.org/):\n";

  return;
}

#define DEFAULT_OUTPUT_PATH "a.out"
static inline
std::string DetermineOutputFilename(const std::string &pOutputPath) {
  if (!pOutputPath.empty()) {
    return pOutputPath;
  }

  // User does't specify the value to -o
  if (OptInputObjectFiles.size() > 1) {
    llvm::errs() << "Use " DEFAULT_OUTPUT_PATH " for output file!\n";
    return DEFAULT_OUTPUT_PATH;
  }

  // There's only one input file
  const std::string &input_path = OptInputObjectFiles[0];
  llvm::SmallString<200> output_path(input_path);

  llvm::error_code err = llvm::sys::fs::make_absolute(output_path);
  if (llvm::errc::success != err) {
    llvm::errs() << "Failed to determine the absolute path of `" << input_path
                 << "'! (detail: " << err.message() << ")\n";
    return "";
  }

  llvm::sys::path::remove_filename(output_path);
  llvm::sys::path::append(output_path, "a.out");

  return output_path.c_str();
}

static inline
bool ConfigLinker(Linker &pLinker, const std::string &pOutputFilename) {
  LinkerConfig* config = NULL;

#ifdef TARGET_BUILD
  config = new (std::nothrow) DefaultLinkerConfig();
#else
  config = new (std::nothrow) LinkerConfig(OptTargetTriple);
#endif
  if (config == NULL) {
    llvm::errs() << "Out of memory when create the linker configuration!\n";
    return false;
  }

  // Setup the configuration accroding to the command line options.

  // 1. Set up soname.
  if (!OptSOName.empty()) {
    config->setSOName(OptSOName);
  } else {
    config->setSOName(pOutputFilename);
  }

  // 2. If given, set up sysroot.
  if (!OptSysRoot.empty()) {
    config->setSysRoot(OptSysRoot);
  }

  // 3. If given, set up dynamic linker path.
  if (!OptDyld.empty()) {
    config->setDyld(OptDyld);
  }

  // 4. If given, set up wrapped symbols.
  llvm::cl::list<std::string>::iterator wrap, wrap_end = OptWrapList.end();
  for (wrap = OptWrapList.begin(); wrap != wrap_end; ++wrap) {
    config->addWrap(*wrap);
  }

  // 5. If given, set up portable symbols.
  llvm::cl::list<std::string>::iterator portable, portable_end = OptPortableList.end();
  for (portable = OptPortableList.begin(); portable != portable_end; ++portable) {
    config->addPortable(*portable);
  }

  // 6. if given, set up search directories.
  llvm::cl::list<std::string>::iterator sdir, sdir_end = OptSearchDirList.end();
  for (sdir = OptSearchDirList.begin(); sdir != sdir_end; ++sdir) {
    config->addSearchDir(*sdir);
  }

  // set up default search directories
  config->addSearchDir("=/lib");
  config->addSearchDir("=/usr/lib");

  // 7. Set up output's type.
  config->setShared(OptShared);

  // 8. Set up -Bsymbolic.
  config->setBsymbolic(OptBsymbolic);

  Linker::ErrorCode result = pLinker.config(*config);
  if (Linker::kSuccess != result) {
    llvm::errs() << "Failed to configure the linker! (detail: "
                 << Linker::GetErrorString(result) << ")\n";
    return false;
  }

  return true;
}

static inline
bool PrepareInputOutput(Linker &pLinker, const std::string &pOutputPath) {
  // -----  Set output  ----- //

  // FIXME: Current MCLinker requires one to set up output before inputs. The
  // constraint will be relaxed in the furture.
  Linker::ErrorCode result = pLinker.setOutput(pOutputPath);

  if (Linker::kSuccess != result) {
    llvm::errs() << "Failed to open the output file! (detail: "
                 << pOutputPath << ": "
                 << Linker::GetErrorString(result) << ")\n";
    return false;
  }

  // -----  Set inputs  ----- //
  llvm::cl::list<std::string>::iterator file_it = OptInputObjectFiles.begin();
  llvm::cl::list<std::string>::iterator lib_it  = OptNameSpecList.begin();

  llvm::cl::list<std::string>::iterator file_begin = OptInputObjectFiles.begin();
  llvm::cl::list<std::string>::iterator lib_begin = OptNameSpecList.begin();
  llvm::cl::list<std::string>::iterator file_end = OptInputObjectFiles.end();
  llvm::cl::list<std::string>::iterator lib_end = OptNameSpecList.end();

  unsigned lib_pos = 0, file_pos = 0;
  while (true) {
    if (lib_it != lib_end) {
      lib_pos = OptNameSpecList.getPosition(lib_it - lib_begin);
    } else {
      lib_pos = 0;
    }

    if (file_it != file_end) {
      file_pos = OptInputObjectFiles.getPosition(file_it - file_begin);
    } else {
      file_pos = 0;
    }

    if ((file_pos != 0) && ((lib_pos == 0) || (file_pos < lib_pos))) {
      result = pLinker.addObject(*file_it);
      if (Linker::kSuccess != result) {
        llvm::errs() << "Failed to open the input file! (detail: " << *file_it
                     << ": " << Linker::GetErrorString(result) << ")\n";
        return false;
      }
      ++file_it;
    } else if ((lib_pos != 0) && ((file_pos == 0) || (lib_pos < file_pos))) {
      result = pLinker.addNameSpec(*lib_it);
      if (Linker::kSuccess != result) {
        llvm::errs() << "Failed to open the namespec! (detail: " << *lib_it
                     << ": " << Linker::GetErrorString(result) << ")\n";
        return false;
      }
      ++lib_it;
    } else {
      break; // we're done with the list
    }
  }

  return true;
}

static inline bool LinkFiles(Linker &pLinker) {
  Linker::ErrorCode result = pLinker.link();
  if (Linker::kSuccess != result) {
    llvm::errs() << "Failed to linking! (detail: "
                 << Linker::GetErrorString(result) << "\n";
    return false;
  }
  return true;
}

int main(int argc, char** argv) {
  llvm::cl::SetVersionPrinter(MCLDVersionPrinter);
  llvm::cl::ParseCommandLineOptions(argc, argv);
  init::Initialize();

  std::string OutputFilename = DetermineOutputFilename(OptOutputFilename);
  if (OutputFilename.empty()) {
    return EXIT_FAILURE;
  }

  Linker linker;
  if (!ConfigLinker(linker, OutputFilename)) {
    return EXIT_FAILURE;
  }

  if (!PrepareInputOutput(linker, OutputFilename)) {
    return EXIT_FAILURE;
  }

  if (!LinkFiles(linker)) {
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}

