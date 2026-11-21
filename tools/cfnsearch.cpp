// tools/cfnsearch.cpp
// Fast, agent-friendly C/C++ function declaration, implementation, and usage
// finder across namespaces, `using` declarations, and `#define` macros.
//
// Build & install systemwide:
//   clang++ -std=c++17 -O3 -DNDEBUG tools/cfnsearch.cpp \
//     $(llvm-config --cxxflags) \
//     -L"$(llvm-config --libdir)" \
//     -lclang-cpp \
//     $(llvm-config --libs support option) \
//     -lpthread -ldl -lm -o cfnsearch && \
//   sudo install -m 0755 ./cfnsearch /usr/local/bin/cfnsearch && \
//   rm -f ./cfnsearch

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Lex/MacroInfo.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/Token.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <unistd.h>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace clang;
using namespace clang::tooling;

namespace {

enum class MatchKind { Decl, Impl, Usage };
enum class OutputMode { Summary, Jsonl, Tsv };

struct SearchOptions {
  std::string symbol;
  std::string kind = "all"; // all | decl | impl | usage
  std::string file_filter;
  OutputMode mode = OutputMode::Summary;
  bool include_system = false;
  bool exact_qualified = false;
  bool verbose = false;
  int max_lines_per_file = 4;
};

struct Hit {
  MatchKind kind;
  std::string file;
  unsigned line = 0;
  unsigned col = 0;
  std::string symbol;
  std::string via;
  std::string snippet;

  bool operator==(const Hit &o) const noexcept {
    return kind == o.kind && line == o.line && col == o.col && file == o.file &&
           symbol == o.symbol && via == o.via;
  }
};

struct HitHash {
  size_t operator()(const Hit &h) const noexcept {
    size_t seed = std::hash<int>{}(static_cast<int>(h.kind));
    auto mix = [&seed](size_t v) {
      seed ^= v + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
    };
    mix(std::hash<std::string>{}(h.file));
    mix(std::hash<unsigned>{}(h.line));
    mix(std::hash<unsigned>{}(h.col));
    mix(std::hash<std::string>{}(h.symbol));
    mix(std::hash<std::string>{}(h.via));
    return seed;
  }
};

class LiveProgress {
public:
  LiveProgress(size_t total, bool enabled) : total_(total), enabled_(enabled) {
    if (!enabled_ || total_ == 0)
      return;
    int dup_fd = ::dup(STDERR_FILENO);
    if (dup_fd >= 0) {
      is_tty_ = ::isatty(dup_fd) != 0;
      if (is_tty_) {
        tty_os_ = std::make_unique<llvm::raw_fd_ostream>(dup_fd,
                                                         /*shouldClose=*/true);
      } else {
        ::close(dup_fd);
      }
    }
  }

  void update(StringRef current_file) {
    if (!tty_os_ || total_ == 0)
      return;
    ++current_;
    size_t done = std::min(current_, total_);
    unsigned pct = static_cast<unsigned>((done * 100) / total_);
    constexpr size_t bar_width = 20;
    size_t filled = (done * bar_width) / total_;

    StringRef base = llvm::sys::path::filename(current_file);
    std::string label = base.str();
    if (label.size() > 28) {
      label = "..." + label.substr(label.size() - 25);
    }

    *tty_os_ << "\r\033[2K[";
    for (size_t i = 0; i < bar_width; ++i) {
      *tty_os_ << (i < filled ? '=' : (i == filled ? '>' : ' '));
    }
    *tty_os_ << "] " << done << '/' << total_ << " (" << pct << "%) " << label;
    tty_os_->flush();
  }

  void finish() {
    if (!tty_os_)
      return;
    *tty_os_ << "\r\033[2K";
    tty_os_->flush();
    tty_os_.reset();
  }

  ~LiveProgress() { finish(); }

private:
  size_t total_ = 0;
  size_t current_ = 0;
  bool enabled_ = false;
  bool is_tty_ = false;
  std::unique_ptr<llvm::raw_fd_ostream> tty_os_;
};

class StderrSilencer {
public:
  explicit StderrSilencer(bool active) : active_(active) {
    if (!active_)
      return;
    llvm::errs().flush();
    saved_fd_ = ::dup(STDERR_FILENO);
    int devnull = ::open("/dev/null", O_WRONLY);
    if (saved_fd_ >= 0 && devnull >= 0) {
      ::dup2(devnull, STDERR_FILENO);
      ::close(devnull);
    } else {
      if (saved_fd_ >= 0)
        ::close(saved_fd_);
      if (devnull >= 0)
        ::close(devnull);
      saved_fd_ = -1;
      active_ = false;
    }
  }

  ~StderrSilencer() {
    if (active_ && saved_fd_ >= 0) {
      llvm::errs().flush();
      ::dup2(saved_fd_, STDERR_FILENO);
      ::close(saved_fd_);
    }
  }

private:
  bool active_ = false;
  int saved_fd_ = -1;
};

static const char *kindTag(MatchKind k) noexcept {
  switch (k) {
  case MatchKind::Decl:
    return "decl";
  case MatchKind::Impl:
    return "impl";
  case MatchKind::Usage:
    return "usage";
  }
  return "usage";
}

static std::string shortenPath(const std::string &path,
                               const std::string &cwd) {
  if (!cwd.empty()) {
    std::string prefix = cwd;
    if (prefix.back() != '/')
      prefix.push_back('/');
    if (path.rfind(prefix, 0) == 0 && path.size() > prefix.size()) {
      return path.substr(prefix.size());
    }
  }
  return path;
}

static std::string jsonEscape(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\b':
      out += "\\b";
      break;
    case '\f':
      out += "\\f";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (c < 0x20) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
        out += buf;
      } else {
        out.push_back(static_cast<char>(c));
      }
    }
  }
  return out;
}

static std::string sanitizeSnippet(StringRef line_text) {
  std::string s = line_text.trim().str();
  std::string compact;
  compact.reserve(s.size());
  bool prev_space = false;
  for (char ch : s) {
    if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
      if (!prev_space) {
        compact.push_back(' ');
        prev_space = true;
      }
    } else {
      compact.push_back(ch);
      prev_space = false;
    }
  }
  if (compact.size() > 100) {
    compact.resize(97);
    compact += "...";
  }
  return compact;
}

class HitSink {
public:
  explicit HitSink(const SearchOptions &opts) : opts_(opts) {}

  bool wantKind(MatchKind k) const noexcept {
    if (opts_.kind == "all")
      return true;
    if (opts_.kind == "decl")
      return k == MatchKind::Decl;
    if (opts_.kind == "impl")
      return k == MatchKind::Impl;
    if (opts_.kind == "usage")
      return k == MatchKind::Usage;
    return true;
  }

  void add(MatchKind kind, SourceLocation loc, const SourceManager &SM,
           const std::string &sym_name, const char *via) {
    if (!wantKind(kind) || loc.isInvalid())
      return;

    SourceLocation exp_loc = SM.getExpansionLoc(loc);
    if (exp_loc.isInvalid())
      return;
    if (!opts_.include_system && SM.isInSystemHeader(exp_loc))
      return;

    SourceLocation spell_loc = SM.getSpellingLoc(loc);
    SourceLocation target_loc = spell_loc.isValid() ? spell_loc : exp_loc;

    FileID fid = SM.getFileID(target_loc);
    auto fe = SM.getFileEntryRefForID(fid);
    std::string path;
    if (fe) {
      path = fe->getFileEntry().tryGetRealPathName().str();
      if (path.empty())
        path = SM.getFilename(target_loc).str();
      if (path.empty())
        path = fe->getName().str();
    } else {
      path = SM.getBufferName(target_loc).str();
    }

    if (!opts_.file_filter.empty() &&
        path.find(opts_.file_filter) == std::string::npos) {
      return;
    }

    unsigned line = SM.getSpellingLineNumber(target_loc);
    unsigned col = SM.getSpellingColumnNumber(target_loc);
    if (line == 0)
      return;

    Hit h;
    h.kind = kind;
    h.file = std::move(path);
    h.line = line;
    h.col = col;
    h.symbol = sym_name;
    h.via = via;

    bool invalid = false;
    StringRef buf = SM.getBufferData(fid, &invalid);
    if (!invalid) {
      const char *buf_start = buf.data();
      const char *loc_ptr = SM.getCharacterData(target_loc);
      if (loc_ptr >= buf_start && loc_ptr <= buf.data() + buf.size()) {
        size_t offset = static_cast<size_t>(loc_ptr - buf_start);
        size_t line_start = buf.rfind('\n', offset);
        line_start = (line_start == StringRef::npos) ? 0 : line_start + 1;
        size_t line_end = buf.find('\n', offset);
        if (line_end == StringRef::npos)
          line_end = buf.size();
        h.snippet =
            sanitizeSnippet(buf.substr(line_start, line_end - line_start));
      }
    }

    if (seen_.insert(h).second) {
      hits_.push_back(std::move(h));
    }
  }

  void emit(llvm::raw_ostream &os, const std::string &cwd) {
    std::sort(hits_.begin(), hits_.end(), [](const Hit &a, const Hit &b) {
      return std::tie(a.file, a.line, a.col, a.kind, a.symbol, a.via) <
             std::tie(b.file, b.line, b.col, b.kind, b.symbol, b.via);
    });

    if (opts_.mode == OutputMode::Jsonl) {
      for (const auto &h : hits_) {
        std::string f = shortenPath(h.file, cwd);
        os << "{\"k\":\"" << kindTag(h.kind) << "\",\"f\":\"" << jsonEscape(f)
           << "\",\"l\":" << h.line << ",\"c\":" << h.col << ",\"s\":\""
           << jsonEscape(h.symbol) << "\",\"v\":\"" << jsonEscape(h.via)
           << "\",\"x\":\"" << jsonEscape(h.snippet) << "\"}\n";
      }
      return;
    }

    if (opts_.mode == OutputMode::Tsv) {
      for (const auto &h : hits_) {
        std::string f = shortenPath(h.file, cwd);
        os << kindTag(h.kind) << '\t' << f << ':' << h.line << ':' << h.col
           << '\t' << h.symbol << '\t' << h.via << '\t' << h.snippet << '\n';
      }
      return;
    }

    size_t decl_cnt = 0, impl_cnt = 0, usage_cnt = 0;
    for (const auto &h : hits_) {
      if (h.kind == MatchKind::Decl)
        ++decl_cnt;
      else if (h.kind == MatchKind::Impl)
        ++impl_cnt;
      else
        ++usage_cnt;
    }

    os << opts_.symbol << " | impl:" << impl_cnt << " decl:" << decl_cnt
       << " usage:" << usage_cnt << "\n";

    if (hits_.empty()) {
      os << "none\n";
      return;
    }

    auto emitSection = [&](MatchKind target_kind, const char *header) {
      std::map<std::string, std::vector<const Hit *>> by_file;
      size_t total = 0;
      for (const auto &h : hits_) {
        if (h.kind == target_kind) {
          by_file[shortenPath(h.file, cwd)].push_back(&h);
          ++total;
        }
      }
      if (total == 0)
        return;

      os << header << " (" << total << ")\n";
      for (const auto &kv : by_file) {
        const std::string &file = kv.first;
        const auto &list = kv.second;
        os << file << "\n";
        size_t shown = std::min<size_t>(list.size(), opts_.max_lines_per_file);
        for (size_t i = 0; i < shown; ++i) {
          const Hit *h = list[i];
          os << "  " << h->line;
          if (h->symbol != opts_.symbol) {
            os << " (" << h->symbol << ')';
          }
          if (h->via != "direct") {
            os << " [" << h->via << ']';
          }
          if (!h->snippet.empty()) {
            os << ": " << h->snippet;
          }
          os << "\n";
        }
        if (list.size() > shown) {
          os << "  +" << (list.size() - shown) << " more: ";
          for (size_t i = shown; i < list.size(); ++i) {
            if (i > shown)
              os << ',';
            os << list[i]->line;
          }
          os << "\n";
        }
      }
    };

    emitSection(MatchKind::Impl, "[IMPL]");
    emitSection(MatchKind::Decl, "[DECL]");
    emitSection(MatchKind::Usage, "[USAGE]");
  }

private:
  const SearchOptions &opts_;
  std::unordered_set<Hit, HitHash> seen_;
  std::vector<Hit> hits_;
};

class SymbolMatcher {
public:
  explicit SymbolMatcher(std::string query, bool exact_qualified)
      : query_(std::move(query)), exact_(exact_qualified) {
    auto pos = query_.rfind("::");
    if (pos != std::string::npos) {
      has_ns_ = true;
      unqualified_ = query_.substr(pos + 2);
      normalized_query_ =
          (query_.rfind("::", 0) == 0) ? query_.substr(2) : query_;
    } else {
      has_ns_ = false;
      unqualified_ = query_;
      normalized_query_ = query_;
    }
  }

  bool matchesQualifiedName(const std::string &qname) const {
    std::string norm_q = qname;
    if (norm_q.rfind("::", 0) == 0)
      norm_q.erase(0, 2);

    if (exact_)
      return norm_q == normalized_query_;
    if (has_ns_) {
      if (norm_q == normalized_query_)
        return true;
      std::string suffix = "::" + normalized_query_;
      return norm_q.size() > suffix.size() &&
             norm_q.compare(norm_q.size() - suffix.size(), suffix.size(),
                            suffix) == 0;
    }
    auto p = norm_q.rfind("::");
    std::string base = (p == std::string::npos) ? norm_q : norm_q.substr(p + 2);
    return base == unqualified_;
  }

  bool matchesUnqualifiedToken(StringRef tok_text) const {
    return tok_text == unqualified_;
  }

private:
  std::string query_;
  std::string normalized_query_;
  std::string unqualified_;
  bool has_ns_ = false;
  bool exact_ = false;
};

class MacroTracker : public PPCallbacks {
public:
  MacroTracker(Preprocessor &PP, const SourceManager &SM,
               const SymbolMatcher &matcher, HitSink &sink)
      : PP_(PP), SM_(SM), matcher_(matcher), sink_(sink) {}

  void MacroDefined(const Token &MacroNameTok,
                    const MacroDirective *MD) override {
    if (!MD || !MD->getMacroInfo())
      return;
    const MacroInfo *MI = MD->getMacroInfo();
    std::string mname = PP_.getSpelling(MacroNameTok);
    bool macro_is_target = matcher_.matchesUnqualifiedToken(mname);

    bool aliases_target = false;
    for (const Token &tok : MI->tokens()) {
      if (tok.is(tok::raw_identifier) || tok.is(tok::identifier)) {
        std::string spelling;
        if (tok.is(tok::raw_identifier)) {
          spelling = tok.getRawIdentifier().str();
        } else if (const IdentifierInfo *II = tok.getIdentifierInfo()) {
          spelling = II->getName().str();
        }
        if (!spelling.empty() && matcher_.matchesUnqualifiedToken(spelling)) {
          aliases_target = true;
          break;
        }
      }
    }

    if (macro_is_target || aliases_target) {
      if (macro_is_target)
        matched_macros_.insert(mname);
      if (aliases_target)
        aliased_macros_.insert(mname);
      sink_.add(MatchKind::Decl, MacroNameTok.getLocation(), SM_, mname,
                macro_is_target ? "macro-def" : "macro-alias");
    }
  }

  void MacroExpands(const Token &MacroNameTok, const MacroDefinition &,
                    SourceRange Range, const MacroArgs *) override {
    std::string mname = PP_.getSpelling(MacroNameTok);
    SourceLocation loc = Range.getBegin().isValid()
                             ? Range.getBegin()
                             : MacroNameTok.getLocation();
    if (matched_macros_.count(mname)) {
      sink_.add(MatchKind::Usage, loc, SM_, mname, "macro-expand");
    } else if (aliased_macros_.count(mname)) {
      sink_.add(MatchKind::Usage, loc, SM_, mname, "macro-alias");
    }
  }

private:
  Preprocessor &PP_;
  const SourceManager &SM_;
  const SymbolMatcher &matcher_;
  HitSink &sink_;
  std::unordered_set<std::string> matched_macros_;
  std::unordered_set<std::string> aliased_macros_;
};

class FunctionFinderVisitor
    : public RecursiveASTVisitor<FunctionFinderVisitor> {
public:
  FunctionFinderVisitor(ASTContext &Ctx, const SymbolMatcher &matcher,
                        HitSink &sink)
      : Ctx_(Ctx), SM_(Ctx.getSourceManager()), matcher_(matcher), sink_(sink) {
  }

  bool VisitFunctionDecl(FunctionDecl *FD) {
    if (!FD || !FD->getDeclName().isIdentifier())
      return true;
    std::string qname = FD->getQualifiedNameAsString();
    if (!matcher_.matchesQualifiedName(qname))
      return true;

    target_decls_.insert(FD->getCanonicalDecl());
    SourceLocation loc = FD->getLocation();
    if (DeclarationNameInfo DNI = FD->getNameInfo(); DNI.getLoc().isValid()) {
      loc = DNI.getLoc();
    }

    if (FD->isThisDeclarationADefinition()) {
      sink_.add(MatchKind::Impl, loc, SM_, qname, "direct");
    } else {
      sink_.add(MatchKind::Decl, loc, SM_, qname, "direct");
    }
    return true;
  }

  bool VisitUsingDecl(UsingDecl *UD) {
    if (!UD)
      return true;
    for (const UsingShadowDecl *USD : UD->shadows()) {
      const NamedDecl *TD = USD->getTargetDecl();
      if (!TD)
        continue;
      std::string target_qname = TD->getQualifiedNameAsString();
      if (matcher_.matchesQualifiedName(target_qname)) {
        if (const auto *FD = dyn_cast<FunctionDecl>(TD)) {
          target_decls_.insert(FD->getCanonicalDecl());
        }
        sink_.add(MatchKind::Usage, UD->getLocation(), SM_, target_qname,
                  "using-decl");
      }
    }
    return true;
  }

  bool VisitDeclRefExpr(DeclRefExpr *DRE) {
    if (!DRE)
      return true;
    const ValueDecl *VD = DRE->getDecl();
    const NamedDecl *FD_found = DRE->getFoundDecl();
    const FunctionDecl *FD = dyn_cast_or_null<FunctionDecl>(VD);
    if (!FD)
      FD = dyn_cast_or_null<FunctionDecl>(FD_found);
    if (!FD)
      return true;

    std::string qname = FD->getQualifiedNameAsString();
    bool is_target = target_decls_.count(FD->getCanonicalDecl()) > 0 ||
                     matcher_.matchesQualifiedName(qname);
    if (!is_target)
      return true;

    SourceLocation loc = DRE->getLocation();
    const char *via = "direct";
    if (isa_and_nonnull<UsingShadowDecl>(FD_found)) {
      via = "using-shadow";
    } else if (loc.isMacroID()) {
      via = "macro-expand";
    }

    sink_.add(MatchKind::Usage, loc, SM_, qname, via);
    return true;
  }

  bool VisitMemberExpr(MemberExpr *ME) {
    if (!ME)
      return true;
    const auto *FD = dyn_cast_or_null<FunctionDecl>(ME->getMemberDecl());
    if (!FD)
      return true;

    std::string qname = FD->getQualifiedNameAsString();
    if (!matcher_.matchesQualifiedName(qname) &&
        target_decls_.count(FD->getCanonicalDecl()) == 0) {
      return true;
    }

    SourceLocation loc =
        ME->getMemberLoc().isValid() ? ME->getMemberLoc() : ME->getExprLoc();
    const char *via = loc.isMacroID() ? "macro-expand" : "direct";
    sink_.add(MatchKind::Usage, loc, SM_, qname, via);
    return true;
  }

private:
  ASTContext &Ctx_;
  const SourceManager &SM_;
  const SymbolMatcher &matcher_;
  HitSink &sink_;
  std::unordered_set<const FunctionDecl *> target_decls_;
};

class FinderConsumer : public ASTConsumer {
public:
  FinderConsumer(ASTContext &Ctx, const SymbolMatcher &matcher, HitSink &sink)
      : visitor_(Ctx, matcher, sink) {}

  void HandleTranslationUnit(ASTContext &Ctx) override {
    visitor_.TraverseDecl(Ctx.getTranslationUnitDecl());
  }

private:
  FunctionFinderVisitor visitor_;
};

class FinderAction : public ASTFrontendAction {
public:
  FinderAction(const SymbolMatcher &matcher, HitSink &sink, bool verbose,
               LiveProgress &progress)
      : matcher_(matcher), sink_(sink), verbose_(verbose), progress_(progress) {
  }

  bool BeginSourceFileAction(CompilerInstance &CI) override {
    progress_.update(getCurrentFile());
    if (!verbose_) {
      DiagnosticsEngine &DE = CI.getDiagnostics();
      DE.setClient(new IgnoringDiagConsumer(), true);
    }
    Preprocessor &PP = CI.getPreprocessor();
    const SourceManager &SM = CI.getSourceManager();
    PP.addPPCallbacks(std::make_unique<MacroTracker>(PP, SM, matcher_, sink_));
    return true;
  }

  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                 StringRef) override {
    return std::make_unique<FinderConsumer>(CI.getASTContext(), matcher_,
                                            sink_);
  }

private:
  const SymbolMatcher &matcher_;
  HitSink &sink_;
  bool verbose_;
  LiveProgress &progress_;
};

class FinderActionFactory : public FrontendActionFactory {
public:
  FinderActionFactory(const SymbolMatcher &matcher, HitSink &sink, bool verbose,
                      LiveProgress &progress)
      : matcher_(matcher), sink_(sink), verbose_(verbose), progress_(progress) {
  }

  std::unique_ptr<FrontendAction> create() override {
    return std::make_unique<FinderAction>(matcher_, sink_, verbose_, progress_);
  }

private:
  const SymbolMatcher &matcher_;
  HitSink &sink_;
  bool verbose_;
  LiveProgress &progress_;
};

static bool isSourceFilePath(StringRef p) {
  StringRef ext = llvm::sys::path::extension(p);
  return ext.equals_insensitive(".c") || ext.equals_insensitive(".cc") ||
         ext.equals_insensitive(".cpp") || ext.equals_insensitive(".cxx") ||
         ext.equals_insensitive(".c++");
}

static bool isIgnoredDir(StringRef p) {
  for (auto it = llvm::sys::path::begin(p), end = llvm::sys::path::end(p);
       it != end; ++it) {
    if (*it == ".git" || *it == "build" || *it == "out" ||
        *it == "node_modules" || *it == "third_party") {
      return true;
    }
  }
  return false;
}

static void collectSourceFiles(const std::string &target,
                               std::vector<std::string> &out) {
  if (!llvm::sys::fs::exists(target))
    return;
  if (llvm::sys::fs::is_directory(target)) {
    std::error_code ec;
    for (llvm::sys::fs::recursive_directory_iterator it(target, ec), end;
         it != end && !ec; it.increment(ec)) {
      if (isIgnoredDir(it->path())) {
        it.no_push();
        continue;
      }
      auto st = it->status();
      if (st && llvm::sys::fs::is_regular_file(*st) &&
          isSourceFilePath(it->path())) {
        out.push_back(it->path());
      }
    }
  } else if (isSourceFilePath(target)) {
    out.push_back(target);
  }
}

static std::string detectClangResourceDir() {
  auto clang_bin = llvm::sys::findProgramByName("clang");
  if (!clang_bin)
    return "";
  SmallString<256> tmp;
  llvm::sys::fs::createTemporaryFile("cfnsearch_res", "txt", tmp);
  std::string out_path = tmp.str().str();
  StringRef args[] = {*clang_bin, "-print-resource-dir"};
  std::optional<StringRef> redirects[] = {std::nullopt, StringRef(out_path),
                                          std::nullopt};
  int rc = llvm::sys::ExecuteAndWait(*clang_bin, args, std::nullopt, redirects);
  std::string res;
  if (rc == 0) {
    auto buf = llvm::MemoryBuffer::getFile(out_path);
    if (buf) {
      res = buf.get()->getBuffer().trim().str();
    }
  }
  llvm::sys::fs::remove(out_path);
  return res;
}

} // namespace

static llvm::cl::OptionCategory CfnSearchCategory("cfnsearch");

static llvm::cl::opt<std::string> PosSymbol(llvm::cl::Positional,
                                            llvm::cl::desc("<symbol>"),
                                            llvm::cl::Optional,
                                            llvm::cl::cat(CfnSearchCategory));

static llvm::cl::list<std::string>
    PosPaths(llvm::cl::Positional, llvm::cl::desc("[files_or_dirs...]"),
             llvm::cl::ZeroOrMore, llvm::cl::cat(CfnSearchCategory));

static llvm::cl::opt<std::string>
    OptCompDb("p", llvm::cl::desc("Path to compile_commands.json or build dir"),
              llvm::cl::init(""), llvm::cl::cat(CfnSearchCategory));

static llvm::cl::opt<std::string>
    OptKind("k", llvm::cl::desc("Filter: all | decl | impl | usage"),
            llvm::cl::init("all"), llvm::cl::cat(CfnSearchCategory));

static llvm::cl::opt<std::string>
    OptFile("f", llvm::cl::desc("Only show hits matching file substring"),
            llvm::cl::init(""), llvm::cl::cat(CfnSearchCategory));

static llvm::cl::opt<bool>
    OptJsonl("jsonl", llvm::cl::desc("Output compact JSONL instead of summary"),
             llvm::cl::init(false), llvm::cl::cat(CfnSearchCategory));

static llvm::cl::opt<bool>
    OptTsv("tsv", llvm::cl::desc("Output TSV instead of summary"),
           llvm::cl::init(false), llvm::cl::cat(CfnSearchCategory));

static llvm::cl::opt<bool>
    OptExact("exact", llvm::cl::desc("Exact qualified namespace match"),
             llvm::cl::init(false), llvm::cl::cat(CfnSearchCategory));

static llvm::cl::opt<bool>
    OptVerbose("v", llvm::cl::desc("Print Clang compiler errors/warnings"),
               llvm::cl::init(false), llvm::cl::cat(CfnSearchCategory));

static llvm::cl::opt<int>
    OptLimit("n",
             llvm::cl::desc("Max lines shown per file in summary (default 4)"),
             llvm::cl::init(4), llvm::cl::cat(CfnSearchCategory));

int main(int argc, const char **argv) {
  llvm::cl::HideUnrelatedOptions(CfnSearchCategory);
  llvm::cl::ParseCommandLineOptions(
      argc, argv,
      "Usage: cfnsearch <symbol> [files_or_dirs...] [-p "
      "compile_commands.json]\n");

  if (PosSymbol.getValue().empty()) {
    llvm::errs() << "usage: cfnsearch <symbol> [files_or_dirs...] [-p "
                    "compile_commands.json]\n";
    return 1;
  }

  SearchOptions opts;
  opts.symbol = PosSymbol.getValue();
  opts.kind = OptKind.getValue();
  opts.file_filter = OptFile.getValue();
  opts.exact_qualified = OptExact.getValue();
  opts.verbose = OptVerbose.getValue();
  opts.max_lines_per_file = std::max(1, OptLimit.getValue());
  if (OptJsonl.getValue())
    opts.mode = OutputMode::Jsonl;
  else if (OptTsv.getValue())
    opts.mode = OutputMode::Tsv;

  std::unique_ptr<CompilationDatabase> CDB;
  std::string err;

  std::string db_path = OptCompDb.getValue();
  if (db_path.empty()) {
    if (llvm::sys::fs::exists("compile_commands.json")) {
      db_path = "compile_commands.json";
    } else if (llvm::sys::fs::exists("build/compile_commands.json")) {
      db_path = "build";
    }
  }

  if (!db_path.empty()) {
    CDB = CompilationDatabase::loadFromDirectory(db_path, err);
    if (!CDB && opts.verbose) {
      llvm::errs() << "note: compile db not loaded (" << err
                   << "), using default flags\n";
    }
  }

  if (!CDB) {
    std::vector<std::string> base_args = {"-fsyntax-only"};
    std::string res_dir = detectClangResourceDir();
    if (!res_dir.empty()) {
      base_args.push_back("-resource-dir=" + res_dir);
    }
    if (llvm::sys::fs::is_directory("include")) {
      base_args.push_back("-Iinclude");
    }
    CDB = std::make_unique<FixedCompilationDatabase>(".", base_args);
  }

  std::vector<std::string> sources;
  if (PosPaths.empty()) {
    collectSourceFiles(".", sources);
  } else {
    for (const auto &p : PosPaths) {
      collectSourceFiles(p, sources);
    }
  }

  if (sources.empty()) {
    llvm::errs() << "no C/C++ source files found\n";
    return 1;
  }

  std::sort(sources.begin(), sources.end());
  sources.erase(std::unique(sources.begin(), sources.end()), sources.end());

  ClangTool Tool(*CDB, sources);

  std::string res_dir = detectClangResourceDir();
  if (!res_dir.empty()) {
    Tool.appendArgumentsAdjuster(
        [res_dir](const CommandLineArguments &args, StringRef) {
          CommandLineArguments adjusted = args;
          adjusted.push_back("-resource-dir=" + res_dir);
          return adjusted;
        });
  }

  SymbolMatcher matcher(opts.symbol, opts.exact_qualified);
  HitSink sink(opts);

  bool show_bar = !opts.verbose && (opts.mode == OutputMode::Summary);
  LiveProgress progress(sources.size(), show_bar);
  FinderActionFactory factory(matcher, sink, opts.verbose, progress);

  int rc = 0;
  {
    StderrSilencer silencer(!opts.verbose);
    rc = Tool.run(&factory);
  }
  progress.finish();

  SmallString<256> cwd_buf;
  llvm::sys::fs::current_path(cwd_buf);
  sink.emit(llvm::outs(), cwd_buf.str().str());

  return opts.verbose ? rc : 0;
}
