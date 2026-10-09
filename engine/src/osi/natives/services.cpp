// M2a-2 T7a: the "services and time" native group. One citation comment per binding (natives.md §1.3): the wrapper body
// (the registry `body`), the method it calls, the function that decides, and the asm pin that engine/tests/
// test_osi_services.cpp checks against the exe. A value that comes from an instruction immediate or a data constant is
// cited by instruction address; INFERRED marks a reading that was not run to a conclusion.
#include "lomn/osi/services.h"

#include <cstring>

#include "lomn/fs.h"

namespace lomn::osi {

// ---- string <-> identifier ----

std::string osi_cstring(const std::string& text) {
  const size_t nul = text.find('\0');
  const size_t n = nul == std::string::npos ? text.size() : nul;
  if (n >= 255) throw VmError("kOutOfRange");  // cmp edi,[esi+8] / setae 0x401894-0x401897, SrThrow 0x4018a8
  return text.substr(0, n);
}

uint32_t convert_string_to_id(const std::string& s) {
  if (s.size() != 4) return 0xffffffffu;  // cmp eax,4 (0x5fb06e) ; mov [edi],0xffffffff (0x5fb075)
  // the load at 0x5fb089 reads the four bytes as one little-endian dword; the masks and shifts of 0x5fb08d-0x5fb0b2 put the
  // first byte in the high byte (shl 24 at 0x5fb093, shl 8 at 0x5fb09e, shr 8 at 0x5fb0a9, shr 24 at 0x5fb0b2)
  return static_cast<uint32_t>(static_cast<uint8_t>(s[0])) << 24 | static_cast<uint32_t>(static_cast<uint8_t>(s[1])) << 16 |
         static_cast<uint32_t>(static_cast<uint8_t>(s[2])) << 8 | static_cast<uint32_t>(static_cast<uint8_t>(s[3]));
}

std::string identifier_to_string(uint32_t id) {
  // 0x6178c0-0x61790d: the dword is byte-swapped and stored little-endian, so memory holds the id high byte first; the
  // first assign takes 4 bytes (0x617938), the copy into the result is strlen (0x61796b)
  std::string out;
  for (int shift = 24; shift >= 0; shift -= 8) {
    const char c = static_cast<char>(id >> shift);
    if (c == 0) break;
    out += c;
  }
  return out;
}

// ---- GcTimer ----

GcTimer::GcTimer(uint16_t type, int32_t id, bool start, double duration)
    : GameObject(type, start ? 1 : 0), id_(id), duration_(duration) {
  // GcTimer::GcTimer 0x445840: id [+0xc] from GcEventTrigger 0x517510 (0x4458d2), duration [+0x68] (0x4458fc), [+0x78] = 0
  // (0x445902); Start only if the byte argument is non-zero (0x445906-0x44590f). Start 0x445990: AddTrigger 0x4c2440 once
  // (0x4459c6-0x4459f1). AddTrigger's `inc [this+4]` (0x4c25e5) feeds the trigger list a copy of the reference, and
  // nothing in M2a-2 removes it: the engine holds the started timer once (INFERRED from 0x4c25da-0x4c2619).
  if (start) trigger_added_ = true;
}

// ---- real delta time ----

namespace {
double bits_double(uint64_t b) {
  double d;
  std::memcpy(&d, &b, 8);
  return d;
}
// the .data constants of ScProcessManager::Process, read from the exe by address (test_osi_services.cpp checks each)
const double kInitDt = bits_double(0x3f9110e460000000ull);   // [0x712db0] 0.016666: the previous value while count < 10
const double kGap = bits_double(0x3fe0000000000000ull);      // [0x712db8] 0.5: a delta above it resets
const double kResetDt = bits_double(0x3f9110e460000000ull);  // [0x712dc0] 0.016666
const double kBlend = bits_double(0x3fb1eb8520000000ull);    // [0x712dc8] 0.07
const double kKeep = bits_double(0x3fedc28f60000000ull);     // [0x712dd0] 0.93
const double kCapAbove = bits_double(0x3fb0e56040000000ull); // [0x712dd8] 0.066
const double kCapTo = bits_double(0x3fb0e56040000000ull);    // [0x712de0] 0.066
}  // namespace

double RealDeltaTime::step(double now) {
  double prev = smoothed;                          // fld [0x712d70] ; fstp [0x712d80]            0x4a920c-0x4a9212
  if (count < 10) prev = kInitDt;                  // cmp [0x712d88],10 ; jge ; fld [0x712db0]    0x4a9218-0x4a9227
  const double delta = now - last_real;            // fld cur ; fsub [pm+0x10]                    0x4a922d-0x4a9239
  double nv;
  if (delta > kGap) nv = kResetDt;                 // fcomp [0x712db8] ; test ah,0x45 ; jne       0x4a923c-0x4a9255
  else nv = delta * kBlend + kKeep * prev;         // fmul [0x712dc8] ; fmul [0x712d80] ; faddp  0x4a9257-0x4a9278
  ++count;                                         // inc [0x712d88]                              0x4a9280
  if (nv > kCapAbove) nv = kCapTo;                 // fcomp [0x712dd8] ; fld [0x712de0]           0x4a928c-0x4a92a5
  smoothed = nv;                                   // fstp [0x712d70]
  last_real = now;                                 // fstp [pm+0x10]                              0x4a92ec-0x4a92f2
  return nv;
}

// ---- block files ----

bool BlockFiles::exists(const std::string& name) {
  sage::BlockChain* chain = ctx_.block_chain();  // Art.blk opened there (path 0x703523, LoadArtBlockHeader 0x43b030)
  if (!chain) return false;  // no volumes mounted: FileExists finds nothing (its no-volume error is caught, 0x54df60)
  return chain->exists(name);
}

bool does_file_exist(HostContext& ctx, const std::string& name) {
  // BetterSaver's hook 0x10001da0: strstr(path, "User/") (0x10001dbe, needle 0x1000833c; case sensitive). A hit never reaches
  // SrBlockFileExists: ResolveGamePath(path, buf, 0x104, false) (ijl15.dll 0x10001570) fills buf, and GetFileAttributesA(buf)
  // (0x10001df2) must find a file that is not a directory (0x10001df8-0x10001e01).
  //  - "Root/" at the start of the path (strstr == path, 0x100015b2): GetCurrentDirectoryA, then the path from after "Root"
  //    appended: the game directory = the VFS's root/ mount.
  //  - "User/" at the start (0x100015cc): SHGetFolderPathA(APPDATA) + the edition + the path from after "User": the user
  //    directory = the VFS's user/ mount.
  //  - NEITHER (0x10001608): sprintf of the "Fatal: We disallow ..." text and MessageBoxA(0, text, "OSI Script Security
  //    Abort", MB_ICONERROR) (0x10001632); there is no exit, execution falls through to the common tail (0x1000163e):
  //    strcat_s(buf, size, path) with the path UNCHANGED, '/' -> '\', ret. buf holds the relative path, GetFileAttributesA
  //    resolves it against the current directory, which is the directory the Root/ branch maps through GetCurrentDirectoryA.
  //    So the answer is exists("Root/" + path). The message box is a host UI effect: not modelled, not logged (natives.md 9.3).
  // Win32 resolution of ".." and the 0x104-byte buffer are not modelled (natives.md 9.3): the VFS normalises ".." inside its
  // mount and never escapes it.
  if (name.find("User/") != std::string::npos) {
    const fs::FileSystem* vfs = ctx.vfs();
    if (!vfs) return false;
    if (name.compare(0, 5, "Root/") == 0 || name.compare(0, 5, "User/") == 0) return vfs->exists(name);
    return vfs->exists("Root/" + name);
  }
  return ctx.subsystem<BlockFiles>().exists(name);  // the original: 0x615ef0 -> SrBlockFileExists 0x54ddc0
}

// ---- the native bodies ----

namespace {
HostContext& host(void* user) { return *static_cast<HostContext*>(user); }
std::string string_arg(HostContext& ctx, NativeCall& c, size_t i) {
  if (i >= c.args.size() || c.args[i]->type != kString) throw VmError("Undefined: string argument expected");
  return osi_cstring(ctx.heap().text(c.args[i]->value));
}

// body 0x617700, method 0x617810 <ScSLOSIFoundation::StringToIdentifier> → 0x5fb060 <ScOSISystem::ConvertStringToID>; asm
// pin: 0x5fb093/0x5fb09e/0x5fb0a9/0x5fb0b2 shift immediates (first character in the high byte); the wrapper's validity flag
// (cmp [0x755798], 0x617882) is discarded, the id is stored (0x61788b)
Variant n_stringtoidentifier(NativeCall& c, void* user) {
  return make_int(static_cast<int32_t>(convert_string_to_id(string_arg(host(user), c, 0))));
}

// body 0x6177a0, method 0x6178c0 <ScSLOSIFoundation::IdentifierToString>; asm pin: 0x6178d7-0x617908 mask/shift immediates
// (a byte swap of the dword, stored little-endian: the id's high byte first), 0x617938 push 4, 0x61796b npos copy; there is
// no table and no unknown-id text in the function
Variant n_identifiertostring(NativeCall& c, void* user) {
  HostContext& ctx = host(user);
  return Variant{kString, ctx.heap().new_string(identifier_to_string(c.args[0]->value))};
}

// body 0x615950, method 0x615ef0 <ScSLOSIFileIO::DoesFileExist> → 0x54ddc0 <SrBlockFileExists> (BetterSaver hook 0x10001da0,
// natives.md §9.3); asm pin: 0x54ddc7 mov ecx,0x83b0b8 (Art.blk), 0x54ddf0 cmp [0x736128],0 (the slot), 0x54de41 call
// FileExists 0x54de70; the "User/" strstr needle at 0x1000833c
Variant n_doesfileexist(NativeCall& c, void* user) {
  HostContext& ctx = host(user);
  return make_int(does_file_exist(ctx, string_arg(ctx, c, 0)) ? 1 : 0);  // CreateVariant 0x407f30: {0x10, movzx byte}
}

// body 0x4454f0, method 0x445840 <GcTimer::GcTimer> → 0x445990 <GcTimer::Start> (AddTrigger 0x4c2440); asm pin: 0x445906-0x44590f
// the Start guard (cmp byte [ebp+0xc],0 : the LOW BYTE of the second argument), 0x4459c6-0x4459f1 Start's AddTrigger, 0x445520
// the float argument passed as a double
Variant n_gctimer_create(NativeCall& c, void* user) {
  HostContext& ctx = host(user);
  const int32_t id = as_int(*c.args[0]);
  const bool start = (c.args[1]->value & 0xff) != 0;  // mov al, byte [edi] (0x445523)
  const double duration = static_cast<double>(as_float(*c.args[2]));  // fld dword / fstp qword (0x44551c-0x445520)
  GcTimer* t = ctx.make<GcTimer>(c.entry.ret_type, id, start, duration);
  return ctx.create_variant(t);  // CreateVariant(GcTimer*) 0x445550: +1 (0x445580)
}

// body 0x4a8ac0, method 0x4a90e0 <ScProcessManager::UseRealDeltaTime>; asm pin: 0x4a90f2 mov [0x712d78], dl (the low byte of
// the argument, 0x4a8aca) and the real-time branch constants 0x712db0-0x712de0. D3: under --deterministic a logged no-op
// (the byte stays 0 and Process keeps the fake time step 0x712d68)
Variant n_userealdeltatime(NativeCall& c, void* user) {
  HostContext& ctx = host(user);
  RealDeltaTime& rt = ctx.subsystem<RealDeltaTime>();
  rt.requested = (c.args[0]->value & 0xff) != 0;
  if (!ctx.flags().deterministic) rt.effective = rt.requested;
  return nothing();  // {0x0f, 0} (0x4a8ad3)
}

// body 0x4367b0, method 0x43ae50 <GcGame::GetResolutionWidth>; asm pin: 0x43ae5a add edx,0x10 / 0x43ae5d fld [edx] (the field
// [[0x70fa64]+0x10]); float through CreateVariant 0x4077f0. The screen size is the INI's ResolutionW (INI → M2a-3): the default
// INI literal "640" (0x7475a2, written at 0x5d4a62)
Variant n_getresolutionwidth(NativeCall&, void* user) { return make_float(host(user).subsystem<DisplayState>().width); }

// body 0x4367d0, method 0x43ae70 <GcGame::GetResolutionHeight>; asm pin: 0x43ae7f fld [edx+4] (the field [[0x70fa64]+0x14]);
// default INI literal "480" (0x7475a6, written at 0x5d4a7c)
Variant n_getresolutionheight(NativeCall&, void* user) { return make_float(host(user).subsystem<DisplayState>().height); }
}  // namespace

void bind_services_group(NativeRegistry& reg, HostContext& ctx) {
  struct Row {
    const char* cls;
    const char* fn;
    NativeFn f;
  };
  static const Row rows[] = {
      {"scslosifoundation", "stringtoidentifier", n_stringtoidentifier},
      {"scslosifoundation", "identifiertostring", n_identifiertostring},
      {"scslosifileio", "doesfileexist", n_doesfileexist},
      {"gctimer", "create", n_gctimer_create},
      {"scprocessmanager", "userealdeltatime", n_userealdeltatime},
      {"gcgame", "getresolutionwidth", n_getresolutionwidth},
      {"gcgame", "getresolutionheight", n_getresolutionheight},
  };
  for (const Row& r : rows)
    if (reg.find(r.cls, r.fn)) reg.bind(std::string(r.cls) + "." + r.fn, r.f, &ctx, Tier::Implemented);
}

}  // namespace lomn::osi
