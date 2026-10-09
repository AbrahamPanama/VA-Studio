// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include "actions/vacards-cli-production.h"
#include "document.h"
#include "inkscape.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <cctype>
#include <vector>
#include <sstream>
#include <cstdlib>
#include <chrono>
#if defined(VACARDS_OPEN_AUDIT_WINDOWS_BACKEND) || defined(VACARDS_OPEN_AUDIT_MACOS_BACKEND)
#if defined(VACARDS_OPEN_AUDIT_WINDOWS_BACKEND)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <mach-o/dyld.h>
#include <dlfcn.h>
#include <fcntl.h>
extern char **environ;
#endif
#include <libxml/parser.h>
#include <lcms2.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <poppler.h>
#include <libcdr/libcdr.h>
#include <librevenge-stream/librevenge-stream.h>
#include "vacards-open-audit/audit.h"
#if defined(VACARDS_OPEN_AUDIT_MACOS_BACKEND)
#include "io/vacards-cli-resources.h"
#endif
#endif
#include <limits>
using namespace Inkscape::VACardsCli;
using namespace boost::json;
namespace {
PackageCommand command(std::string_view id) {
    for (auto const &c : production_commands()) if (c.id==id) return c;
    throw std::logic_error("Missing production descriptor");
}
}
#if !defined(VACARDS_OPEN_AUDIT_WINDOWS_BACKEND) && !defined(VACARDS_OPEN_AUDIT_MACOS_BACKEND)
TEST(M3ParserAudit, RequiredOutcomesNotImplemented) {
    GTEST_SKIP() << "No parser-open audit backend is built on this platform.";
}
#elif defined(VACARDS_OPEN_AUDIT_WINDOWS_BACKEND) || defined(VACARDS_OPEN_AUDIT_MACOS_BACKEND)
namespace {
namespace fs = std::filesystem;
struct OpenEvent { std::string case_id, kind, api, path, access, result, caller; };
struct AuditRun { fs::path root, granted, ungranted, log, executable; std::vector<OpenEvent> events; };
#if defined(VACARDS_OPEN_AUDIT_WINDOWS_BACKEND)
std::wstring widen(std::string const &s) { return std::wstring(s.begin(), s.end()); }
std::string narrow(std::wstring const &s) { return std::string(s.begin(), s.end()); }
#else
std::string narrow(std::string const &s) { return s; }
#endif
std::string env(char const *name) { auto p=std::getenv(name); return p ? p : ""; }
std::string unescape(std::string s) {
    std::string out; for (size_t i=0;i<s.size();++i) {
        if (s[i]=='%' && i+2<s.size()) { unsigned x=0; std::istringstream in(s.substr(i+1,2)); in>>std::hex>>x; out.push_back((char)x); i+=2; }
        else out.push_back(s[i]);
    } return out;
}
std::vector<OpenEvent> read_events(fs::path const &file) {
    std::vector<OpenEvent> out; std::ifstream in(file); std::string line;
    while (std::getline(in,line)) {
        std::istringstream row(line); std::string tag; std::getline(row,tag,'\t'); if(tag!="OPEN") continue;
        OpenEvent e; std::getline(row,e.case_id,'\t'); std::getline(row,e.kind,'\t'); std::getline(row,e.api,'\t');
        std::getline(row,e.path,'\t'); std::getline(row,e.access,'\t'); std::getline(row,e.result,'\t'); std::getline(row,e.caller);
        e.case_id=unescape(e.case_id); e.path=unescape(e.path); e.caller=unescape(e.caller); out.push_back(std::move(e));
    } return out;
}
std::string classify(OpenEvent const &e, AuditRun const &r) {
    auto p=e.path;
#if defined(VACARDS_OPEN_AUDIT_WINDOWS_BACKEND)
    if(p.rfind("\\\\?\\",0)==0) p.erase(0,4);
    std::replace(p.begin(),p.end(),'\\','/');
#endif
#if defined(VACARDS_OPEN_AUDIT_MACOS_BACKEND)
    auto caller=e.caller;
    auto readonly=[](std::string const &value) {
        try { auto flags=std::stoul(value,nullptr,0); return (flags & (O_WRONLY|O_RDWR|O_CREAT|O_TRUNC|O_APPEND))==0; }
        catch (...) { return false; }
    };
    if(e.kind=="network" && e.api=="connect" && e.access=="0x1" && p=="/var/run/syslog"
       && caller=="/usr/lib/system/libsystem_platform.dylib") return "system";
    if(e.kind=="open" && (e.api=="opendir" || e.api=="opendir$INODE64" || e.api=="open" || e.api=="open$NOCANCEL")
       && readonly(e.access)
       && (caller=="/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation" || caller=="/usr/lib/system/libxpc.dylib")
       && (p==r.executable.string() || p==r.executable.parent_path().string())) return "system";
#endif
    std::string normalized=p; std::transform(normalized.begin(),normalized.end(),normalized.begin(),[](unsigned char c){return (char)std::tolower(c);});
    if(normalized.find("vacards-parser-audit-")!=std::string::npos && normalized.find("/granted/")!=std::string::npos) return "granted";
    if(normalized.find("vacards-parser-audit-")!=std::string::npos && normalized.find("/ungranted/")!=std::string::npos) return "ungranted";
    auto under=[&](fs::path const &base){ auto b=base.lexically_normal().string(); std::replace(b.begin(),b.end(),'\\','/'); std::string lb=b; std::transform(lb.begin(),lb.end(),lb.begin(),[](unsigned char c){return (char)std::tolower(c);}); if(normalized.size()<lb.size() || normalized.compare(0,lb.size(),lb)!=0)return false; return normalized.size()==lb.size() || lb.back()=='/' || normalized[lb.size()]=='/'; };
    if (under(r.granted)) return "granted";
    if (under(r.ungranted)) return "ungranted";
    p=std::move(normalized);
    std::ifstream inv(std::string(INKSCAPE_TESTS_DIR) + "/src/vacards-open-audit/" +
#if defined(VACARDS_OPEN_AUDIT_WINDOWS_BACKEND)
                      "windows-runtime-read-inventory.tsv");
#else
                      "macos-runtime-read-inventory.tsv");
#endif
    std::string line; while(std::getline(inv,line)) {
        if(line.empty() || line[0]=='#') continue;
        auto tab=line.find('\t'); if(tab==std::string::npos) continue;
        std::string pat=line.substr(0,tab); std::transform(pat.begin(),pat.end(),pat.begin(),[](unsigned char c){return (char)std::tolower(c);}); std::string reason=line.substr(tab+1);
#if defined(VACARDS_OPEN_AUDIT_MACOS_BACKEND)
        bool matched=(pat=="." || pat=="/") ? p==pat : p.find(pat)!=std::string::npos;
#else
        bool matched=p.find(pat)!=std::string::npos;
#endif
        if(matched && !reason.empty()) return "inventory";
    }
    return "unclassified";
}
std::string p9_escape(std::string const &s) {
    static const char hex[]="0123456789ABCDEF"; std::string out;
    for(unsigned char c:s) { if(c=='%'||c==' '||c=='\t'||c=='\r'||c=='\n') {out.push_back('%');out.push_back(hex[c>>4]);out.push_back(hex[c&15]);} else out.push_back((char)c); }
    return out;
}
void emit_event(AuditRun const &r, OpenEvent const &e) {
    std::cout << "P9-OPEN-EVENT case=" << p9_escape(e.case_id) << " kind=" << e.kind << " api=" << e.api << " path=" << p9_escape(e.path)
              << " access=" << e.access << " result=" << e.result << " caller=" << p9_escape(e.caller)
              << " classification=" << classify(e,r) << "\n";
}
void write_file(fs::path const &p, std::string const &data) { fs::create_directories(p.parent_path()); std::ofstream o(p,std::ios::binary); o.write(data.data(),data.size()); }
AuditRun make_run(std::string const &id) {
    AuditRun r; r.root=fs::temp_directory_path()/fs::path("vacards-parser-audit-"+id+"-"+
#if defined(VACARDS_OPEN_AUDIT_WINDOWS_BACKEND)
        std::to_string(GetCurrentProcessId()));
#else
        std::to_string((long)getpid()));
#endif
    fs::remove_all(r.root); r.granted=r.root/"granted"; r.ungranted=r.root/"ungranted"; r.log=r.root/"events.tsv";
    fs::create_directories(r.granted); fs::create_directories(r.ungranted);
    write_file(r.granted/"control.txt","granted positive control\n");
    write_file(r.ungranted/"control.txt","ungranted control\n");
    write_file(r.ungranted/"outside.svg","<svg xmlns=\"http://www.w3.org/2000/svg\"><rect width=\"1\" height=\"1\"/></svg>");
    write_file(r.ungranted/"external.svg","<!DOCTYPE svg [<!ENTITY outside SYSTEM \"resource.svg\">]><svg xmlns=\"http://www.w3.org/2000/svg\">&outside;</svg>");
    fs::copy_file(fs::path(INKSCAPE_TESTS_DIR)/"src/vacards-open-audit/fixtures/tiny.cdr",r.ungranted/"outside.cdr");
    fs::copy_file(fs::path(INKSCAPE_TESTS_DIR)/"src/vacards-open-audit/fixtures/tiny.pdf",r.ungranted/"outside.pdf");
    fs::copy_file(fs::path(INKSCAPE_TESTS_DIR)/"src/vacards-open-audit/fixtures/tiny.png",r.ungranted/"outside.png");
    fs::copy_file(fs::path(INKSCAPE_TESTS_DIR)/"src/vacards-open-audit/fixtures/external.png",r.ungranted/"resource.png");
    write_file(r.ungranted/"resource.svg","<rect xmlns=\"http://www.w3.org/2000/svg\" width=\"1\" height=\"1\"/>\n");
    auto profile=cmsCreate_sRGBProfile(); if(profile){cmsSaveProfileToFile(profile,(r.ungranted/"outside.icc").string().c_str());cmsCloseProfile(profile);}
    fs::copy_file(fs::path(INKSCAPE_TESTS_DIR)/"src/vacards-open-audit/fixtures/tiny.cdr",r.ungranted/"control-librevenge.cdr");
    fs::copy_file(fs::path(INKSCAPE_TESTS_DIR)/"src/vacards-open-audit/fixtures/tiny.pdf",r.ungranted/"control-poppler.pdf");
    fs::copy_file(fs::path(INKSCAPE_TESTS_DIR)/"src/vacards-open-audit/fixtures/tiny.png",r.ungranted/"control-gdk.png");
    write_file(r.ungranted/"control-libxml.xml","<root/>\n"); write_file(r.ungranted/"control-ifstream.txt","ifstream\n");
    write_file(r.ungranted/"control-glib.txt","glib\n");
#if defined(VACARDS_OPEN_AUDIT_MACOS_BACKEND)
    write_file(r.ungranted/"canonicalization-probe.txt","canonicalization probe\n");
#endif
    return r;
}
bool has_path(AuditRun const &r,std::string const &part) { return std::any_of(r.events.begin(),r.events.end(),[&](auto const &e){return e.path.find(part)!=std::string::npos;}); }
bool run_child(std::string const &case_name,AuditRun &r) {
#if defined(VACARDS_OPEN_AUDIT_WINDOWS_BACKEND)
    WCHAR module[32768]; DWORD n=GetModuleFileNameW(nullptr,module,32768); if(!n||n>=32768)return false;
    std::wstring filter=L" --gtest_filter=M3ParserAudit.DISABLED_"+widen(case_name)+L" --gtest_also_run_disabled_tests";
    std::wstring cmd=L"\""+std::wstring(module,n)+L"\""+filter;
    SetEnvironmentVariableW(L"VACARDS_AUDIT_CHILD",L"1"); SetEnvironmentVariableW(L"VACARDS_AUDIT_CASE",widen(case_name).c_str());
    SetEnvironmentVariableW(L"VACARDS_AUDIT_EVENT_FILE",r.log.wstring().c_str());
    SetEnvironmentVariableW(L"VACARDS_AUDIT_GRANTED",r.granted.wstring().c_str()); SetEnvironmentVariableW(L"VACARDS_AUDIT_UNGRANTED",r.ungranted.wstring().c_str());
    struct SavedEnvironmentVariable { bool present; std::wstring value; };
    auto save_environment=[](LPCWSTR name) {
        DWORD size=GetEnvironmentVariableW(name,nullptr,0);
        if(!size) return SavedEnvironmentVariable{GetLastError()!=ERROR_ENVVAR_NOT_FOUND,L""};
        std::wstring value(size,L'\0'); DWORD length=GetEnvironmentVariableW(name,value.data(),size);
        if(!length) return SavedEnvironmentVariable{GetLastError()!=ERROR_ENVVAR_NOT_FOUND,L""};
        value.resize(length); return SavedEnvironmentVariable{true,value};
    };
    auto gtest_output=save_environment(L"GTEST_OUTPUT"); auto gtest_filter=save_environment(L"GTEST_FILTER");
    SetEnvironmentVariableW(L"GTEST_OUTPUT",nullptr); SetEnvironmentVariableW(L"GTEST_FILTER",nullptr);
    STARTUPINFOW si{}; si.cb=sizeof(si); PROCESS_INFORMATION pi{};
    BOOL ok=CreateProcessW(module,cmd.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi);
    SetEnvironmentVariableW(L"GTEST_OUTPUT",gtest_output.present?gtest_output.value.c_str():nullptr);
    SetEnvironmentVariableW(L"GTEST_FILTER",gtest_filter.present?gtest_filter.value.c_str():nullptr);
    SetEnvironmentVariableW(L"VACARDS_AUDIT_CHILD",nullptr); SetEnvironmentVariableW(L"VACARDS_AUDIT_CASE",nullptr);
    SetEnvironmentVariableW(L"VACARDS_AUDIT_EVENT_FILE",nullptr); SetEnvironmentVariableW(L"VACARDS_AUDIT_GRANTED",nullptr); SetEnvironmentVariableW(L"VACARDS_AUDIT_UNGRANTED",nullptr);
    if(!ok) return false;
    DWORD waited=WaitForSingleObject(pi.hProcess,45000);
    DWORD code=999;
    if(waited==WAIT_TIMEOUT) { TerminateProcess(pi.hProcess,124); WaitForSingleObject(pi.hProcess,5000); }
    GetExitCodeProcess(pi.hProcess,&code); CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    if(waited==WAIT_TIMEOUT) return false;
    r.events=read_events(r.log); return code==0;
#else
    uint32_t n=0; _NSGetExecutablePath(nullptr,&n); std::vector<char> exe(n+1); if(_NSGetExecutablePath(exe.data(),&n)!=0)return false;
    std::error_code canonical_error; auto canonical_exe=fs::canonical(exe.data(),canonical_error); if(canonical_error)return false;
    r.executable=canonical_exe;
    std::cout << "P9-AUDIT-EXECUTABLE case=" << case_name << " path=" << p9_escape(canonical_exe.string()) << "\n";
    fs::path interposer=fs::path(exe.data()).parent_path()/"vacards-open-audit.dylib";
    std::vector<std::string> vars; for(char **p=environ;p&&*p;++p)vars.emplace_back(*p);
    vars.erase(std::remove_if(vars.begin(),vars.end(),[](auto const &v){return v.rfind("GTEST_",0)==0;}),vars.end());
    auto put=[&](std::string const &key,std::string const &value){auto prefix=key+"=";auto i=std::find_if(vars.begin(),vars.end(),[&](auto const &v){return v.rfind(prefix,0)==0;});if(i==vars.end())vars.push_back(prefix+value);else *i=prefix+value;};
    put("VACARDS_AUDIT_CHILD","1"); put("VACARDS_AUDIT_CASE",case_name); put("VACARDS_AUDIT_EVENT_FILE",r.log.string());
    put("VACARDS_AUDIT_GRANTED",r.granted.string()); put("VACARDS_AUDIT_UNGRANTED",r.ungranted.string()); put("DYLD_INSERT_LIBRARIES",interposer.string());
    std::vector<char*> envp; for(auto &v:vars)envp.push_back(v.data()); envp.push_back(nullptr);
    std::string filter="--gtest_filter=M3ParserAudit.DISABLED_"+case_name; char *argv[]={exe.data(),filter.data(),(char*)"--gtest_also_run_disabled_tests",nullptr};
    auto child_log=r.root/(case_name+".child.log"); posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions,STDERR_FILENO,child_log.c_str(),O_WRONLY|O_CREAT|O_TRUNC,0600);
    posix_spawn_file_actions_adddup2(&actions,STDERR_FILENO,STDOUT_FILENO);
    pid_t pid=-1; int spawned=posix_spawn(&pid,exe.data(),&actions,nullptr,argv,envp.data()); posix_spawn_file_actions_destroy(&actions);
    if(spawned){std::cerr<<"posix_spawn failed: "<<strerror(spawned)<<" executable="<<exe.data()<<" interposer="<<interposer<<"\n";return false;}
    int status=0; auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(45); pid_t waited=0;
    while((waited=waitpid(pid,&status,WNOHANG))==0 && std::chrono::steady_clock::now()<deadline) usleep(10000);
    if(waited==0) { (void)kill(pid,SIGKILL); (void)waitpid(pid,&status,0); std::cerr<<"parser audit child timed out and was killed: "<<case_name<<"\n"; return false; }
    if(waited<0){std::cerr<<"waitpid failed for parser audit child\n";return false;}
    r.events=read_events(r.log); bool ok=WIFEXITED(status)&&WEXITSTATUS(status)==0;
    if(!ok){std::ifstream child_output(child_log);std::cerr<<"parser audit child status="<<status<<" event_log="<<r.log<<" exists="<<fs::exists(r.log)<<" events="<<r.events.size()<<"\n"<<child_output.rdbuf();}
    return ok;
#endif
}
void run_parent(std::string const &id,std::string const &child,std::string const &expected) {
    auto r=make_run(id); ASSERT_TRUE(run_child(child,r)) << "Parser audit child failed: " << child;
    std::cout << "P9-PARSER-OPEN-CASE id=" << id << " events=" << r.events.size() << "\n";
    for(auto const &e:r.events) emit_event(r,e);
    ASSERT_FALSE(r.events.empty()) << "The hook emitted no open events";
    if(!expected.empty()) {
        auto event=std::find_if(r.events.begin(),r.events.end(),[&](auto const &e){return e.path.find(expected)!=std::string::npos;});
        ASSERT_NE(event,r.events.end()) << "Missing expected event path: " << expected;
        EXPECT_EQ(classify(*event,r),"ungranted") << "Outside-grant event was misclassified";
#if defined(VACARDS_OPEN_AUDIT_MACOS_BACKEND)
        if(id=="CanonicalizationOpen") {
            EXPECT_EQ(event->api,"open");
            EXPECT_NE(event->caller.find("libinkscape_base"),std::string::npos);
        }
#endif
    }
    fs::remove_all(r.root);
}
#if defined(VACARDS_OPEN_AUDIT_WINDOWS_BACKEND)
HMODULE load_audit_api() {
    WCHAR exe[32768]; DWORD n=GetModuleFileNameW(nullptr,exe,32768); if(!n||n>=32768)return nullptr;
    fs::path dll=fs::path(std::wstring(exe,n)).parent_path()/L"vacards-open-audit.dll"; return LoadLibraryW(dll.c_str());
}
#else
void *load_audit_api() { return dlsym(RTLD_DEFAULT,"VacardsOpenAuditStart"); }
#endif
void child_open(std::string const &name) {
#if defined(VACARDS_OPEN_AUDIT_WINDOWS_BACKEND)
    auto dll=load_audit_api(); ASSERT_NE(dll,nullptr) << "LoadLibrary of test-only audit DLL failed";
    auto start=(decltype(&VacardsOpenAuditStartW))GetProcAddress(dll,"VacardsOpenAuditStartW");
    auto stop=(decltype(&VacardsOpenAuditStop))GetProcAddress(dll,"VacardsOpenAuditStop");
    auto coverage=(decltype(&VacardsOpenAuditCheckCoverage))GetProcAddress(dll,"VacardsOpenAuditCheckCoverage");
#else
    auto dll=load_audit_api(); ASSERT_NE(dll,nullptr) << "DYLD interposer is absent; parser opens cannot be observed";
    auto start=(decltype(&VacardsOpenAuditStart))dlsym(RTLD_DEFAULT,"VacardsOpenAuditStart");
    auto stop=(decltype(&VacardsOpenAuditStop))dlsym(RTLD_DEFAULT,"VacardsOpenAuditStop");
    auto coverage=(decltype(&VacardsOpenAuditCheckCoverage))dlsym(RTLD_DEFAULT,"VacardsOpenAuditCheckCoverage");
#endif
    ASSERT_NE(start,nullptr); ASSERT_NE(stop,nullptr); ASSERT_NE(coverage,nullptr);
#if defined(VACARDS_OPEN_AUDIT_WINDOWS_BACKEND)
    auto log=widen(env("VACARDS_AUDIT_EVENT_FILE")); auto id=widen(env("VACARDS_AUDIT_CASE")); ASSERT_TRUE(start(log.c_str(),id.c_str()));
#else
    auto log=env("VACARDS_AUDIT_EVENT_FILE"); auto id=env("VACARDS_AUDIT_CASE"); ASSERT_TRUE(start(log.c_str(),id.c_str()));
#endif
    EXPECT_TRUE(coverage()) << "IAT patch failed while auditing";
    auto root=fs::path(env("VACARDS_AUDIT_UNGRANTED")); auto path=[&](char const *s){
#if defined(VACARDS_OPEN_AUDIT_WINDOWS_BACKEND)
        return (root/s).wstring();
#else
        return (root/s).string();
#endif
    };
    if(name=="OutsideGrantSvg") { auto p=path("outside.svg"); auto d=xmlReadFile(narrow(p).c_str(),nullptr,XML_PARSE_NONET|XML_PARSE_NOERROR|XML_PARSE_NOWARNING); if(d)xmlFreeDoc(d); }
    else if(name=="OutsideGrantCdr") { auto p=path("outside.cdr"); librevenge::RVNGFileStream in(narrow(p).c_str()); (void)libcdr::CDRDocument::isSupported(&in); }
    else if(name=="OutsideGrantPdf" || name=="ControlPoppler") { auto p=path(name=="OutsideGrantPdf"?"outside.pdf":"control-poppler.pdf"); auto uri=g_filename_to_uri(narrow(p).c_str(),nullptr,nullptr); GError *err=nullptr; auto d=poppler_document_new_from_file(uri,nullptr,&err); if(d)g_object_unref(d); if(err)g_error_free(err); g_free(uri); }
    else if(name=="OutsideGrantImage" || name=="ControlGdk") { auto p=path(name=="OutsideGrantImage"?"outside.png":"control-gdk.png"); GError *err=nullptr; auto b=gdk_pixbuf_new_from_file(narrow(p).c_str(),&err); if(b)g_object_unref(b); if(err)g_error_free(err); }
    else if(name=="SvgExternalResource") { auto p=path("external.svg"); auto d=xmlReadFile(narrow(p).c_str(),nullptr,XML_PARSE_NONET|XML_PARSE_NOENT|XML_PARSE_DTDLOAD|XML_PARSE_NOERROR|XML_PARSE_NOWARNING); if(d)xmlFreeDoc(d); }
    else if(name=="ControlLibxml") { auto p=path("control-libxml.xml"); auto d=xmlReadFile(narrow(p).c_str(),nullptr,XML_PARSE_NONET|XML_PARSE_NOERROR|XML_PARSE_NOWARNING); if(d)xmlFreeDoc(d); }
    else if(name=="ControlLcms") { auto p=path("outside.icc"); auto c=cmsOpenProfileFromFile(narrow(p).c_str(),"r"); if(c)cmsCloseProfile(c); }
    else if(name=="ControlLibrevenge") { auto p=path("control-librevenge.cdr"); librevenge::RVNGFileStream in(narrow(p).c_str()); (void)in.isEnd(); }
    else if(name=="ControlIfstream") { std::ifstream in(fs::path(path("control-ifstream.txt")),std::ios::binary); std::string x; in>>x; }
    else if(name=="ControlGlib") { gchar *data=nullptr; gsize size=0; g_file_get_contents(narrow(path("control-glib.txt")).c_str(),&data,&size,nullptr); g_free(data); }
#if defined(VACARDS_OPEN_AUDIT_MACOS_BACKEND)
    else if(name=="CanonicalizationOpen") {
        Inkscape::VACardsCli::Grants no_grants;
        auto access=Inkscape::VACardsCli::inspect_command_path(path("canonicalization-probe.txt"),no_grants);
        ASSERT_EQ(access.state,"ungranted");
    }
#endif
    else if(name=="NoUngrantedParserOpens" || name=="HookAbsentIsNotObserved") {
        auto p=(fs::path(env("VACARDS_AUDIT_GRANTED"))/"control.txt");
#if defined(VACARDS_OPEN_AUDIT_WINDOWS_BACKEND)
        auto w=p.wstring(); HANDLE h=CreateFileW(w.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr); ASSERT_NE(h,INVALID_HANDLE_VALUE); CloseHandle(h);
#else
        int h=::open(p.c_str(),O_RDONLY); ASSERT_GE(h,0); ::close(h);
#endif
    }
    else if(name=="ControlDirect") { auto p=(fs::path(env("VACARDS_AUDIT_GRANTED"))/"control.txt");
#if defined(VACARDS_OPEN_AUDIT_WINDOWS_BACKEND)
        auto w=p.wstring(); HANDLE h=CreateFileW(w.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr); ASSERT_NE(h,INVALID_HANDLE_VALUE); CloseHandle(h);
#else
        int h=::open(p.c_str(),O_RDONLY); ASSERT_GE(h,0); ::close(h);
#endif
    }
    else if(name=="InterceptorCoverageControls") {
        auto direct=(fs::path(env("VACARDS_AUDIT_GRANTED"))/"control.txt");
#if defined(VACARDS_OPEN_AUDIT_WINDOWS_BACKEND)
        auto wd=direct.wstring(); HANDLE h=CreateFileW(wd.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr); if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);
#else
        int h=::open(direct.c_str(),O_RDONLY); if(h>=0)::close(h);
#endif
        auto xml=path("control-libxml.xml"); auto doc=xmlReadFile(narrow(xml).c_str(),nullptr,XML_PARSE_NONET|XML_PARSE_NOERROR|XML_PARSE_NOWARNING); if(doc)xmlFreeDoc(doc);
        auto icc=path("outside.icc"); auto profile=cmsOpenProfileFromFile(narrow(icc).c_str(),"r"); if(profile)cmsCloseProfile(profile);
        GError *err=nullptr; auto pix= gdk_pixbuf_new_from_file(narrow(path("control-gdk.png")).c_str(),&err); if(pix)g_object_unref(pix); if(err)g_error_free(err);
        auto uri=g_filename_to_uri(narrow(path("control-poppler.pdf")).c_str(),nullptr,nullptr); err=nullptr; auto pdf=poppler_document_new_from_file(uri,nullptr,&err); if(pdf)g_object_unref(pdf); if(err)g_error_free(err); g_free(uri);
        { librevenge::RVNGFileStream stream(narrow(path("control-librevenge.cdr")).c_str()); (void)stream.isEnd(); }
        { std::ifstream in(fs::path(path("control-ifstream.txt")),std::ios::binary); std::string x; in>>x; }
        gchar *data=nullptr; gsize size=0; g_file_get_contents(narrow(path("control-glib.txt")).c_str(),&data,&size,nullptr); g_free(data);
    }
    /* The IAT remains patched until child exit, so keep the hook DLL resident. */
    stop();
}
}
TEST(M3ParserAudit, OutsideGrantSvg) { run_parent("OutsideGrantSvg","OutsideGrantSvg","outside.svg"); }
TEST(M3ParserAudit, OutsideGrantCdr) { run_parent("OutsideGrantCdr","OutsideGrantCdr","outside.cdr"); }
TEST(M3ParserAudit, OutsideGrantPdf) { run_parent("OutsideGrantPdf","OutsideGrantPdf","outside.pdf"); }
TEST(M3ParserAudit, OutsideGrantImage) { run_parent("OutsideGrantImage","OutsideGrantImage","outside.png"); }
TEST(M3ParserAudit, SvgExternalResource) { run_parent("SvgExternalResource","SvgExternalResource","resource.svg"); }
#if defined(VACARDS_OPEN_AUDIT_MACOS_BACKEND)
TEST(M3ParserAudit, CanonicalizationOpen) { run_parent("CanonicalizationOpen","CanonicalizationOpen","canonicalization-probe.txt"); }
#endif
TEST(M3ParserAudit, InterceptorCoverageControls) {
    auto r=make_run("InterceptorCoverageControls");
    ASSERT_TRUE(run_child("InterceptorCoverageControls",r));
    const char *controls[]={"control.txt","control-libxml.xml","outside.icc","control-gdk.png","control-poppler.pdf",
        "control-librevenge.cdr","control-ifstream.txt","control-glib.txt"};
    std::cout << "P9-PARSER-OPEN-CASE id=InterceptorCoverageControls events=" << r.events.size() << "\n";
    for(auto const &e:r.events)emit_event(r,e);
    for(auto const *c:controls) {
        auto event=std::find_if(r.events.begin(),r.events.end(),[&](auto const &e){return e.path.find(c)!=std::string::npos;});
        ASSERT_NE(event,r.events.end()) << "Missed forbidden-open control at " << c;
        EXPECT_EQ(classify(*event,r),std::string(c)=="control.txt"?"granted":"ungranted")
            << "Wrong grant classification for control " << c;
    }
    fs::remove_all(r.root);
}
TEST(M3ParserAudit, HookAbsentIsNotObserved) {
    auto r=make_run("HookAbsentIsNotObserved"); ASSERT_TRUE(run_child("HookAbsentIsNotObserved",r));
    auto control=std::find_if(r.events.begin(),r.events.end(),[](auto const &e){return e.path.find("granted\\control.txt")!=std::string::npos || e.path.find("granted/control.txt")!=std::string::npos;});
    ASSERT_NE(control,r.events.end()) << "A missing hook leaves the positive control unobserved and must fail this test";
    EXPECT_EQ(classify(*control,r),"granted");
    std::cout << "P9-PARSER-OPEN-CASE id=HookAbsentIsNotObserved events=" << r.events.size() << " hook=required observed=true\n";
    for(auto const &e:r.events) emit_event(r,e);
    fs::remove_all(r.root);
}
TEST(M3ParserAudit, NoUngrantedParserOpens) {
    auto r=make_run("NoUngrantedParserOpens"); ASSERT_TRUE(run_child("NoUngrantedParserOpens",r));
    std::cout << "P9-PARSER-OPEN-CASE id=NoUngrantedParserOpens events=" << r.events.size() << "\n";
    for(auto const &e:r.events) {emit_event(r,e); EXPECT_NE(classify(e,r),"ungranted"); EXPECT_NE(classify(e,r),"unclassified");
#if defined(VACARDS_OPEN_AUDIT_MACOS_BACKEND)
        EXPECT_NE(e.kind,"spawn"); EXPECT_NE(e.kind,"exec"); EXPECT_NE(e.kind,"fork"); EXPECT_TRUE(e.kind!="network" || classify(e,r)=="system");
#endif
    }
    EXPECT_TRUE(has_path(r,"granted\\control.txt") || has_path(r,"granted/control.txt")); fs::remove_all(r.root);
}
#define AUDIT_CHILD_TEST(name) TEST(M3ParserAudit, DISABLED_##name) { if(env("VACARDS_AUDIT_CHILD")!="1") GTEST_SKIP()<<"Child-only audit scenario"; child_open(#name); }
AUDIT_CHILD_TEST(OutsideGrantSvg)
AUDIT_CHILD_TEST(OutsideGrantCdr)
AUDIT_CHILD_TEST(OutsideGrantPdf)
AUDIT_CHILD_TEST(OutsideGrantImage)
AUDIT_CHILD_TEST(SvgExternalResource)
#if defined(VACARDS_OPEN_AUDIT_MACOS_BACKEND)
AUDIT_CHILD_TEST(CanonicalizationOpen)
#endif
AUDIT_CHILD_TEST(InterceptorCoverageControls)
AUDIT_CHILD_TEST(HookAbsentIsNotObserved)
AUDIT_CHILD_TEST(NoUngrantedParserOpens)
#endif
TEST(M3Admission, RequiredExactUint64Seed) {
    auto c=command("nest.solve");
    auto p=c.example; p["seed"]=std::numeric_limits<std::uint64_t>::max();
    object normalized;
    ASSERT_FALSE(normalize_schema_params(p,c.spec.input->schema,normalized));
    EXPECT_TRUE(normalized.at("seed").is_uint64());
    EXPECT_EQ(normalized.at("seed").as_uint64(),std::numeric_limits<std::uint64_t>::max());
    for (auto raw : {"18446744073709551616", "1e3", "1.0", "-1"}) {
        p["seed"]=parse(raw); EXPECT_TRUE(normalize_schema_params(p,c.spec.input->schema,normalized)) << raw;
    }
}
TEST(M3Admission, RequiredConvertedCssPxLimit) {
    auto c=command("geometry.offset"); object normalized;
    auto p=c.example; p["distance"]=object{{"value",2},{"unit","in"}};
    ASSERT_FALSE(normalize_schema_params(p,c.spec.input->schema,normalized));
    EXPECT_EQ(normalized.at("distance").as_object().at("unit"),value("px"));
    EXPECT_DOUBLE_EQ(normalized.at("distance").as_object().at("value").to_number<double>(),192);
    p["distance"]=object{{"value",20000},{"unit","in"}};
    EXPECT_TRUE(normalize_schema_params(p,c.spec.input->schema,normalized));
    // A nested recipe length uses the same recursive descriptor, not just top-level ParamSpec.
    auto schema=object{{"type","object"},{"properties",object{{"recipe",object{{"type","object"},
        {"properties",object{{"distance",c.spec.input->schema.at("properties").as_object().at("distance")}}}}}}}};
    EXPECT_TRUE(normalize_schema_params(object{{"recipe",p}},schema,normalized));
}
TEST(M3Admission, RequiredDefaultsAfterRoute) {
    object normalized;
    auto copy=command("bitmap.copy"); auto p=copy.example;
    p.erase("dpi"); p["size"]=object{{"width",120},{"height",80}};
    ASSERT_FALSE(normalize_schema_params(p,copy.spec.input->schema,normalized));
    EXPECT_FALSE(normalized.contains("dpi"));
    p["dpi"]=96; EXPECT_TRUE(normalize_schema_params(p,copy.spec.input->schema,normalized));
    auto apply=command("nest.apply");
    ASSERT_FALSE(normalize_schema_params(object{{"solution-token","token"}},apply.spec.input->schema,normalized));
    EXPECT_FALSE(normalized.contains("analyze")); EXPECT_FALSE(normalized.contains("solve"));
    EXPECT_TRUE(normalize_schema_params({},apply.spec.input->schema,normalized));
    EXPECT_TRUE(normalize_schema_params(object{{"solution-token","token"},{"analyze",object{}}},apply.spec.input->schema,normalized));
}
TEST(M3Admission, RequiredGuardDomain) {
    if (!Inkscape::Application::exists()) Inkscape::Application::create(false,Inkscape::Application::RuntimePolicy::PreviewHelper);
    std::string svg=R"(<svg xmlns="http://www.w3.org/2000/svg"/>)";
    auto doc=SPDocument::createNewDocFromMem(std::span<char const>(svg.data(),svg.size())); ASSERT_TRUE(doc);
    auto stamp=document_stamp(doc.get());
    DispatchContext context; context.document=doc.get(); context.session_revision=23;
    unsigned called=0; context.production_handler=[&](Request const &){ ++called; return Record{}; };
    Request r; r.id="test"; r.command="selection.set"; r.document=stamp.id; r.if_revision=23; r.params={{"ids",array{}}};
    auto result=dispatch_production(r,context); EXPECT_EQ(result.status,Status::Ok); EXPECT_EQ(called,1u);
    r.if_revision=24; EXPECT_EQ(dispatch_production(r,context).reason,"stale-revision"); EXPECT_EQ(called,1u);
    r.command="history.query"; r.params={}; r.if_revision.reset();
    EXPECT_EQ(dispatch_production(r,context).status,Status::Ok); EXPECT_EQ(called,2u);
    r.if_revision=stamp.revision+1; EXPECT_EQ(dispatch_production(r,context).reason,"stale-revision");
    r.if_revision=stamp.revision; context.cancelled=[] { return true; };
    EXPECT_EQ(dispatch_production(r,context).status,Status::Cancelled); EXPECT_EQ(called,2u);
    context.cancelled={}; context.command_admission=[](Request const &)->std::optional<Record>{
        Record r; r.status=Status::Rejected; r.reason="document-read-only"; return r;
    };
    EXPECT_EQ(dispatch_production(r,context).reason,"document-read-only"); EXPECT_EQ(called,2u);
}
TEST(M3Admission, DuplicateIdsExclusiveMinimumAndPropertyNames) {
    auto c=command("selection.set");
    EXPECT_EQ(validate_schema(object{{"ids",array{"a","a"}}},c.spec.input->schema)->code,"invalid-argument");
    EXPECT_FALSE(validate_schema(object{{"ids",array{}}},c.spec.input->schema));
    object positive{{"type","number"},{"exclusiveMinimum",0}};
    EXPECT_TRUE(validate_schema(0,positive)); EXPECT_FALSE(validate_schema(0.01,positive));
    object names{{"type","object"},{"propertyNames",object{{"minLength",1}}}};
    EXPECT_TRUE(validate_schema(object{{"",1}},names));
}

TEST(M3Admission, EveryExampleNormalizesWithoutInventingRoutes) {
    for (auto const &c : production_commands()) {
        object normalized;
        auto error=normalize_schema_params(c.example,c.spec.input->schema,normalized);
        ASSERT_FALSE(error) << c.id << ": " << (error ? error->message : "");
        EXPECT_FALSE(validate_schema(normalized,c.spec.input->schema)) << c.id;
        if (c.id=="bitmap.explode.explode") {
            EXPECT_FALSE(normalized.contains("contour")); EXPECT_FALSE(normalized.contains("contour-style"));
            EXPECT_FALSE(normalized.at("recipe").as_object().empty());
        }
    }
}

TEST(M3Admission, RawOverlayParserPreservesExactSeedAndRejectsAmbiguousKeys) {
    auto c=command("nest.solve");
    auto p=c.example; p["seed"]=std::numeric_limits<std::uint64_t>::max();
    object envelope{{"schema","va-studio.cli-request/1"},{"id","raw"},{"command","nest.solve"},
        {"document","doc"},{"params",p}};
    auto parsed=parse_production_request(serialize(envelope)); ASSERT_TRUE(parsed.request);
    EXPECT_EQ(parsed.request->params.at("seed").as_uint64(),std::numeric_limits<std::uint64_t>::max());
    EXPECT_FALSE(parse_request(serialize(envelope)).request); // shipped registry still unavailable
    p["seed"]=1.0; envelope["params"]=p;
    auto rejected=parse_production_request(serialize(envelope)); ASSERT_TRUE(rejected.error);
    EXPECT_EQ(rejected.error->code,"invalid-argument"); EXPECT_EQ(rejected.error_id,"raw");
    auto duplicate=parse_production_request(R"({"schema":"va-studio.cli-request/1","id":"a","id":"b","command":"history.query","params":{},"document":"doc"})");
    ASSERT_TRUE(duplicate.error); EXPECT_EQ(duplicate.error->code,"repeated-key"); EXPECT_FALSE(duplicate.request);
}

TEST(M3Admission, NullableResultsPatternsAndExactUniqueIntegers) {
    object nullable{{"type",array{"string","null"}}};
    EXPECT_FALSE(validate_schema(nullptr,nullable)); EXPECT_FALSE(validate_schema("label",nullable));
    EXPECT_TRUE(validate_schema(1,nullable));
    object hex{{"type","string"},{"pattern","^#[0-9a-fA-F]{6}$"}};
    EXPECT_FALSE(validate_schema("#fA0123",hex)); EXPECT_TRUE(validate_schema("#xx0123",hex));
    auto max=std::numeric_limits<std::uint64_t>::max();
    EXPECT_TRUE(validate_schema(max,object{{"type","integer"},{"maximum",max-1}}));
    object unique{{"type","array"},{"uniqueItems",true}};
    EXPECT_FALSE(validate_schema(array{max,max-1},unique)); EXPECT_TRUE(validate_schema(array{max,max},unique));
}

TEST(M3Admission, OverlayParserPreservesLegacyControlAdmission) {
    auto request=R"({"schema":"va-studio.cli-request/1","id":"status","command":"session.status","params":{}})";
    auto old=parse_request(request); auto overlay=parse_production_request(request);
    ASSERT_TRUE(old.request); ASSERT_TRUE(overlay.request);
    EXPECT_EQ(overlay.request->command,old.request->command);
    EXPECT_EQ(overlay.request->params,old.request->params);
    auto invalid=R"({"schema":"va-studio.cli-request/1","id":"status","command":"session.status","params":{"bogus":true}})";
    auto old_error=parse_request(invalid); auto overlay_error=parse_production_request(invalid);
    ASSERT_TRUE(old_error.error); ASSERT_TRUE(overlay_error.error);
    EXPECT_EQ(old_error.error->code,overlay_error.error->code);
    EXPECT_TRUE(m3_accepted);
}
