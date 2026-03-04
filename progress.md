# Progress Log

## 2026-03-04

### Step 41: TinyCC TLS Runtime Bring-up Fixes (real `__thread` execution)
- Root-cause fix: TLS init-image source in dynamic loader
  - Fixed `/lib/ld-furios.so` PT_TLS handling to copy TLS initializer bytes from ELF file image (`p_offset`) instead of runtime `p_vaddr`.
  - This fixes crashes when TLS sections are emitted in a PT_TLS segment that is not part of PT_LOAD mappings.
  - File:
    - `user/ld-furios.c`

- Kernel context safety for TLS base register:
  - Extended trapframe with `tpidr_el0` and updated vector save/restore paths so TP is preserved across exceptions and task switches.
  - Files:
    - `include/task.h`
    - `kernel/vectors.S`

- AArch64 TinyCC backend TLS codegen correctness:
  - Fixed `arm64_sym_tls()` register-clobber bug when destination register is `x30`.
  - Previous sequence overwrote the loaded TLS offset with `TPIDR_EL0`, producing invalid `tp + tp` addresses.
  - File:
    - `third_party/tinycc/arm64-gen.c`

- Tooling/test coverage improvements:
  - Added explicit TLS regression flow to `test.sh`:
    - positive: cross-object `__thread` compile/link/run (`tls-ok`)
    - negative: block-scope `_Thread_local` must error.
  - Added strict TLS runtime failure gates (`exec failed: /mnt/tls.elf` and trap pattern).
  - Expanded `/bin/elfinfo` diagnostics (dynamic tags, TLS symbol dump, RELA dump) for ELF/TLS debugging.
  - Files:
    - `test.sh`
    - `user/elfinfo.c`

- Validation:
  - `make -j4` PASS.
  - `./test.sh` PASS.

- External references used for this pass:
  - GNU assembler AArch64 relocation operators (TLS GOTTPREL forms):
    - https://sourceware.org/binutils/docs/as/AArch64_002dRelocations.html
  - GCC TLS model notes (`__thread`, TLS semantics):
    - https://gcc.gnu.org/onlinedocs/gcc/Thread-Local.html
  - Arm ABI release index (AAELF64/AArch64 ELF ABI context):
    - https://github.com/ARM-software/abi-aa/releases

### Step 38: Coherency/Durability/Loader/I-O Hardening Pass
- Unified page-cache + `mmap` coherency improvements:
  - Added range invalidation API: `pagecache_invalidate_inode_range(inode, off, len)`.
  - Wired `msync(..., MS_INVALIDATE)` to invalidate eligible cache lines after range writeback.
  - Safety rule: pinned pages (active task mappings/refcount > 1) are never dropped from cache metadata.
- Ext4 JBD2 replay validation tightened:
  - During replay, journal superblock records now require checksum validity.
  - Replay now rejects superblock records injected mid-open transaction (`tx_open`/pending updates/revokes).
- Dynamic loader compatibility uplift (`/lib/ld-furios.so`):
  - Added `LD_LIBRARY_PATH` search before `/lib` and `/usr/lib` for `DT_NEEDED` resolution.
  - Expanded auxv capacity and switched to pass-through + override model:
    - preserves incoming auxv keys by default,
    - overrides runtime-critical keys (`AT_PHDR/AT_PHNUM/AT_ENTRY/AT_BASE/...`),
    - sets `AT_EXECFN` to the real target program path.
- NVMe/AHCI production diagnostics:
  - Added classified transport error accounting and periodic condensed diagnostics for NVMe submit failures.
  - Added AHCI per-port error counters (timeout/TFES/busy) and recovery counting diagnostics.
- Regression coverage:
  - Added short `msync` test in `test.sh` that validates `MS_SYNC|MS_INVALIDATE` on a shared mapped ext4 file.

### Step 37b: Escape Parser Deadlock Fix (Nano Input Unwedge)
- Fixed remaining `nano` deadlock case where partial `ESC` sequences could block input forever:
  - Added timed byte reads for escape-sequence follow-up bytes using `poll` (`read_byte_timeout`).
  - Lone/incomplete `ESC` now falls back safely instead of waiting forever, so controls like quit continue to work.
- Added paste-mode fail-safe:
  - `ESC` during paste now cancels paste mode and restores normal editing path.
- Restored regular screen refresh in main loop to avoid “looks frozen” behavior while processing long paste streams.
- Validation:
  - `make -j4` passed.
  - `./test.sh` passed end-to-end after parser timeout changes.

### Step 36b: Nano Mid-Paste Stall Root-Cause Hardening
- Addressed interactive `-nographic` terminal failure mode where editor appeared to “freeze/crash” during paste:
  - Reworked bracketed paste handling from blocking read loop to non-blocking start/end state machine (`KEY_BRACKETED_PASTE` / `KEY_BRACKETED_PASTE_END`).
  - Added private CSI consume path (`ESC [ ? ...`) to avoid escape-stream desync.
  - Reduced paste-path redraw churn by skipping full-screen refresh while paste mode is active.
- Improved editor behavior/safety:
  - New files now initialize with one empty row (no confusing `0 line(s)` buffer state).
  - Save path switched to temp-write + `fsync` + `rename` to avoid truncation/data-loss windows.
  - Updated key UX to terminal-safe defaults: `Ctrl+O` save, `Ctrl+X` quit (legacy `Ctrl+S`/`Ctrl+Q` aliases still accepted).
- Hardened launcher TTY behavior in `run.sh`:
  - Disable `ixon/ixoff` for interactive sessions while QEMU runs.
  - Restore original TTY settings on exit/signal.
- Validation:
  - `make -j4` passed.
  - `./test.sh` passed (including nano bracketed-paste regression + ext4/tcc matrix).

### Step 35: Nano Paste Crash Hardening + Durability Regression Coverage
- Hardened `user/nano.c` escape parsing:
  - Added robust CSI numeric sequence parsing (`ESC [ ...`) instead of fixed 3-byte parsing.
  - Added explicit bracketed paste detection (`ESC [ 200~` start, `ESC [ 201~` end).
- Added safe bracketed paste ingest path:
  - Paste content now inserts as regular text/newlines/tabs and exits cleanly on `201~`.
  - Prevented raw control-stream leakage into editor buffer during paste bursts.
- Hardened editor memory/state handling:
  - Added line/row limits (`NANO_MAX_LINE_CHARS`, `NANO_MAX_ROWS`).
  - Reworked row insertion to avoid partial-state mutation on allocation failure.
  - Added reserve overflow bounds and status reporting on allocation/size failures.
  - Added cursor/viewport clamp guards and safe row draw bounds handling.
- Improved save durability:
  - `editor_save()` now calls `fsync(fd)` before close to reduce data-loss windows on crashes/power loss.
- Added automated regression in `test.sh`:
  - Opens `nano`, injects bracketed paste stream, saves, exits, and verifies pasted file content.
  - Keeps runtime short while directly covering the previously failing workflow.
- Validation:
  - `make -j4` passed.
  - `./test.sh` passed end-to-end (ext4 + tcc + nano paste flow + AHCI probe).

## 2026-03-01

### Step 1: Project Bootstrap
- Created full freestanding AArch64 kernel/userspace tree.
- Added Makefile, linker scripts (`linker.ld`, `user/user.ld`), `run.sh`, and `test.sh`.
- Added UART, string, print, PMM bump allocator, MMU bring-up, vectors, trap dispatch.

### Step 2: EL Transition Bring-up
- Implemented `EL3 -> EL2 -> EL1` and direct `EL2 -> EL1` flow in `kernel/boot.S`.
- Set required control registers for clean drop and AArch64 EL1 execution.
- Fixed early boot fault by aligning `__bss_start`/`__bss_end` symbols correctly.

### Step 3: MMU + Tables
- Configured `MAIR_EL1`, `TCR_EL1`, `TTBR0_EL1`, `TTBR1_EL1`, `SCTLR_EL1`.
- Implemented L1 and L2 block descriptor helpers separately (1GB vs 2MB).
- Mapped RAM and UART MMIO correctly after MMU enable.

### Step 4: Process + Syscall + ELF
- Implemented task table, trapframe, round-robin scheduler, and context handoff.
- Implemented syscall ABI (`x8` nr, `x0-x5` args, `x0` retval) with dispatcher table.
- Added ELF loader for EL0 process images from embedded user ELFs.
- Implemented `fork`, `exec`, `wait`, `exit`, `getpid`, `yield`.

### Step 5: In-memory VFS + Userland
- Implemented in-memory tree (`/`, `/bin`, `/etc`) and mutable files.
- Added FD model and `open/read/write/close` paths.
- Embedded and ran EL0 programs: init/sh/ls/cat/echo.
- Shell supports parse, fork, exec, wait.

### Step 6: Runtime Debug + Stabilization
- Fixed DTB overlap issue by linking kernel at `0x40200000`.
- Fixed trapframe restore bug (base register corruption in restore path).
- Fixed `exec` return clobber (preserve argc/argv regs for new image).
- Enabled FP/SIMD in `CPACR_EL1` for EL0 binaries generated by GCC.
- Updated init to persist as PID 1 and respawn shell.

### Step 7: Test Automation
- Added `test.sh` smoke test for boot + interactive command flow.
- Verified command sequence in QEMU:
  - `ls`
  - `cat /etc/motd`
  - `echo ...`
  - shell restart via `exit`

### Step 8: Stability + Shell UX Fixes
- Fixed `fork failed` exhaustion root cause:
  - Added page free/reuse support (`pmm_free_page`) with free-list recycling.
  - Added task resource teardown on reap/failed fork/failed exec setup.
  - Fixed blocking wait semantics (`sys_wait` now blocks correctly in userspace wrapper, no lost reap).
- Added real current-working-directory support:
  - New syscall `SYS_CHDIR`.
  - Kernel path resolution now supports relative paths against per-task `cwd`.
  - `cwd` is inherited across `fork`.
- Added shell builtins:
  - `cd`
  - `clear`
  - protected `init` invocation from shell (`init` remains managed by PID 1).
- Improved `ls` behavior:
  - default path changed from `/` to `.` so it respects current directory.
- Updated run mode to rely on `-nographic` + `-monitor none` without explicit `-serial stdio`.
- Re-ran stress loops (60+ repeated `ls`) to validate no more `fork failed`.

### Step 9: EL0 Admin Utilities + VFS Mutations
- Added EL0 binaries in `/bin`:
  - `clear`
  - `mkdir`
  - `rmdir`
  - `rm` (supports `-r`, `-f`, `-rf`)
- Removed shell builtin `clear` so `clear` now executes as real EL0 userspace binary.
- Implemented new syscalls:
  - `SYS_MKDIR`
  - `SYS_RMDIR`
  - `SYS_UNLINK`
- Added per-task relative-path support already introduced via `cwd`/`chdir`, and extended syscall path resolution to directory mutation syscalls.
- Extended in-memory FS with mutation APIs:
  - directory creation
  - file unlink
  - empty-directory removal
- Added inode slot reuse in FS pool to support repeated create/remove cycles.
- Expanded automated regression test coverage (`test.sh`) to include:
  - `clear`
  - `cd /bin` + `ls`
  - `mkdir` + `rm -rf` lifecycle
  - post-cleanup root listing checks

### Step 10: Timer IRQ Preemption + More EL0 Tools
- Replaced syscall-driven timeslicing with timer interrupt preemption:
  - Added GICv2 driver (`kernel/gic.c`) with distributor + CPU interface setup.
  - Added generic timer driver (`kernel/timer.c`) using `cntfrq_el0`, `cntp_tval_el0`, `cntp_ctl_el0`.
  - Enabled 100Hz periodic tick and wired IRQ handling through GIC acknowledge/EOI.
  - Preemption now occurs on timer IRQ when interrupted context is EL0.
- Kept syscall scheduling explicit only where required:
  - `exit`
  - blocking `wait` (`-2`)
  - `yield`
- Fixed MMU mapping gap that caused EL1 abort on GIC access:
  - Added device mapping for `0x08000000` GIC region in both kernel TTBR0 template and per-task TTBR0 tables.
- Pinned QEMU machine to GICv2 for this kernel implementation:
  - `-machine virt,virtualization=on,gic-version=2`
- Added new EL0 `/bin` programs:
  - `pwd` (`sys_getcwd`)
  - `touch`
  - `cp`
  - `mv` (`sys_rename`)
- Added new syscalls:
  - `SYS_GETCWD`
  - `SYS_RENAME`
- Added FS rename support (`fs_rename`) with safety checks (root protection, destination collision, directory cycle prevention).
- Fixed `rm -rf` directory mutation bug:
  - Previous logic could skip entries while deleting during directory iteration.
  - Updated `rm` to snapshot directory entries first, then delete.
- Expanded `test.sh` regression flow for:
  - `pwd`
  - `touch`
  - `cp`
  - `mv`
  - directory removal verification (`ls /tmpx` must fail after `rm -rf /tmpx`)

### Step 11: Per-task Kernel Stacks + Shell Parser Upgrade
- Implemented per-task EL1 kernel stacks and stack-pointer handoff on context switch:
  - Added `kstack_base`, `kstack_top`, and trapframe pointer storage in `task_t`.
  - Allocated a dedicated 16KiB kernel stack per task at task allocation time.
  - Freed per-task stack pages on task reap.
  - Moved trapframe state ownership from copied struct to per-task trapframe slot on that task's kernel stack.
- Updated exception return assembly to switch SP to the selected task's kernel stack frame:
  - `RESTORE_FROM_X0` now sets `sp = tf + TF_SIZE` from the returned trapframe pointer.
  - `trap_enter_first` initializes SP from first task trapframe location before first `eret`.
- Added syscall `SYS_DUP2` and userspace wrapper `sys_dup2` to support shell redirection.
- Updated syscall I/O behavior for redirected stdio:
  - `fd 0/1/2` default to UART when not redirected.
  - if redirected via `dup2`, read/write paths use file-backed FDs.
- Upgraded shell parser/executor:
  - quote handling: single and double quotes
  - escape handling with backslash
  - input/output redirection: `<` and `>`
  - pipelines: `|` with staged command chaining
- Updated `cat` to read from stdin when no path is given (enables pipeline input use).
- Expanded regression test script for parser/redirect pipeline behavior:
  - `echo "hello world" > /q1`
  - `cat < /q1`
  - `echo foo\ bar`
  - `echo hi | cat`
  - cleanup via `rm /q1`
- Increased test timeout to keep full command matrix stable in CI-like runs.

### Step 12: Real Kernel Pipes + Concurrent Pipeline Execution
- Implemented kernel pipe subsystem (`kernel/pipe.c`):
  - fixed-size in-memory ring buffer per pipe
  - reader/writer endpoint refcounts
  - EOF and broken-pipe detection based on endpoint liveness
- Added new syscall:
  - `SYS_PIPE` with user ABI `pipe(int fds[2])`
- Extended FD model to support endpoint kinds:
  - inode-backed FDs
  - pipe read end
  - pipe write end
  - default stdio sentinel (`FD_NONE`) for UART-backed console
- Added pipe-aware FD lifecycle:
  - `dup2` now adjusts pipe endpoint refcounts correctly
  - `fork` duplicates FD table and increments pipe endpoint refs
  - `close`/task exit decrements refs and wakes blocked peers
- Added pipe blocking/wakeup integration with scheduler:
  - pipe read on empty with active writers blocks caller (`TASK_WAITING`)
  - pipe write on full with active readers blocks caller
  - peer read/write/close wakes waiting tasks on same pipe
  - syscall wrappers (`sys_read`/`sys_write`) loop on kernel block return `-2`
- Replaced shell staged temporary-file pipelines with real concurrent pipelines:
  - `|` now uses `pipe()+fork()+dup2()+exec`
  - each stage runs concurrently
  - parent closes unused ends and waits for all children
- Kept redirection + quoting behavior from prior step and made it compose with real pipes.
- Expanded test coverage to include:
  - `cat /etc/motd | cat > /p1` then `cat /p1`
  - multi-stage `echo one two | cat | cat`
  - cleanup and stability checks

### Step 13: Centralized Syscall Metadata Table
- Centralized syscall definition metadata in a single authoritative table inside `kernel/syscall.c`:
  - syscall number (`nr`)
  - syscall name (`name`)
  - argument count (`argc`)
  - handler function pointer (`handler`)
- Reworked syscall number source-of-truth into `include/syscall.h`:
  - added `FUROS_SYSCALL_LIST(X)` macro with `(symbol, nr, name, argc)` entries
  - syscall enum is now generated from this list
- Replaced direct sparse function-pointer array dispatch with metadata + handler lookup:
  - dispatch now resolves syscall info through bounded lookup (`nr < SYS_MAX`)
  - added explicit default unknown handler (`sys_unknown_impl`) for out-of-range/unmapped syscalls
  - unknown syscalls cleanly return `-1`
- Added compile-time ABI drift guard:
  - `SYS_MAX` derived from enum generated by `FUROS_SYSCALL_LIST`
  - `_Static_assert` verifies metadata count matches `SYS_MAX`
- Kept syscall ABI behavior unchanged (`x8` number, `x0-x5` args, `x0` return), while making table evolution safer for future additions (`sleep`, `ps`, etc.).
- Re-ran full regression (`make`, `test.sh`) to ensure no behavior regressions and keyboard/interactive flow stability in `-nographic`.

### Step 14: Sleep Syscall + FD/Redirection Hardening
- Extended single-source syscall list in `include/syscall.h`:
  - removed trace-control syscall
  - added `SYS_SLEEP` entry and userspace wrapper `sys_sleep()`
- Implemented kernel sleep blocking on timer ticks:
  - `sys_sleep(ticks)` blocks task with `WAIT_SLEEP`
  - timer IRQ wake path (`task_wake_sleepers(timer_ticks())`) moves expired sleepers back to runnable
  - scheduler reschedules immediately when sleepers wake
  - fixed no-runnable deadlock when all tasks are blocked on `wait()+sleep` by adding scheduler idle polling that wakes sleepers from current timer tick state
- Added EL0 `/bin/sleep` utility:
  - `sleep <seconds>` converts to kernel ticks (`TIMER_HZ`) and blocks in EL0 via SVC syscall.
- Updated tick source in timer driver:
  - `timer_ticks()` now derives ticks from hardware counter (`cntpct_el0`) and configured tick interval
  - sleep deadlines continue progressing even when no timer IRQ has been serviced yet in the current scheduling path

### Step 15: Page Fault Handling + Copy-on-Write Fork
- Implemented COW `fork` in `kernel/task.c`:
  - removed eager full-page copy during `fork`
  - parent/child now share user pages initially
  - writable pages are remapped read-only in both tasks and marked `cow=true`
  - read-only code/data pages are shared read-only without COW write intent
- Added per-physical-page reference counting in PMM (`kernel/pmm.c`):
  - `pmm_ref_page()`
  - `pmm_page_refcount()`
  - `pmm_free_page()` now decrements refcount and only returns page to free-list at zero
- Added fault-time COW resolution:
  - trap sync handler decodes instruction/data abort EC classes and write faults (`WnR`)
  - write fault on COW page allocates private page (when shared), copies data, remaps writable, clears COW flag
  - write fault on single-owner COW page simply flips mapping back to writable
- Added global TLB flush helper (`mmu_tlb_flush_all`) and used it after mapping-permission/ownership transitions.
- Added EL0 regression utility `/bin/cowtest`:
  - verifies child writes after `fork` do not mutate parent memory
  - covers both EL0 direct writes and kernel `copy_to_user` writes in child
- Expanded `test.sh` to execute `cowtest` in standard smoke flow.
- Performed additional stress run of repeated `cowtest` invocations (20x) in QEMU nographic session; no panic, no fork exhaustion, no `no runnable` regression.
- Hardened FD semantics in `sys_open/read/write`:
  - added access-mode validation via `O_ACCMODE`
  - reject invalid/unsupported open flag combinations
  - reject `O_APPEND` with read-only opens
  - enforce read-on-write-only and write-on-read-only failures
  - honor `O_APPEND` on each write by advancing write offset to file end
- Upgraded shell output redirection:
  - retained `>` truncation-by-recreate behavior
  - added `>>` append redirection using `O_APPEND`
- Removed no-longer-needed syscall trace/self-test user tools and smoke checks from tree/tests.

### Step 16: Demand Paging + Targeted TLB Invalidation
- Added demand paging for user translation faults in `task_handle_page_fault`:
  - heap growth range: `[heap_base, heap_limit)`
  - stack growth range: `[stack_base, USER_STACK_TOP)`
  - translation fault in allowed range allocates zeroed page on demand and maps RW, non-exec
  - instruction translation faults are not demand-mapped (execute from unmapped pages still faults)
- Reduced eager stack mapping in ELF load path:
  - only top stack page is pre-mapped
  - additional stack pages are mapped lazily on fault
- Added per-task demand-paging bounds metadata:
  - `heap_base`, `heap_limit`
  - `stack_base`, `stack_mapped_bottom`
- Added targeted TLB invalidation helper:
  - `mmu_tlb_flush_va(va)` using `tlbi vae1`
  - replaced full TLB flushes for single-page COW and demand-map remaps with VA-targeted invalidation
- Added EL0 `/bin/vmtest` for VM fault-path coverage:
  - validates heap demand-mapping with write/read to unmapped heap VA
  - validates stack growth fault handling through deep recursive stack usage
- Extended automated smoke flow:
  - `test.sh` now runs both `cowtest` and `vmtest`
  - increased QEMU timeout budget to keep the longer command matrix stable.

### Step 17: Explicit `brk/sbrk` + Strict Stack Guard Growth
- Added explicit VM syscall `SYS_BRK`:
  - syscall table now includes `brk`
  - kernel handler calls new `task_brk()` path
- Added userspace heap APIs:
  - `sys_brk(void *addr)`
  - `sys_sbrk(long increment)` wrapper built on top of `brk`
- Reworked demand paging heap gating:
  - added per-task `heap_end`
  - translation-fault heap mapping now only allowed below `align_up(heap_end, PAGE_SIZE)`
  - removed broad fault acceptance across the entire `[heap_base, heap_limit)` window
- Implemented `brk` shrink unmap:
  - when heap is contracted, fully out-of-range heap pages are unmapped and freed/refcount-dropped
- Added strict per-process stack guard policy:
  - fault page must be exactly current guard page (`stack_mapped_bottom - PAGE_SIZE`)
  - fault must correspond to current `sp_el0` page, with small slop bound
  - blocks arbitrary stack-range fault mapping and enforces controlled downward growth
- Updated `vmtest` to validate heap growth through `sbrk()` (instead of hardcoded VA writes).

### Step 18: Hardware-PTE COW Validation + ASID-Scoped VA TLB Invalidations
- Reworked user-page software metadata to avoid duplicating live PTE access state:
  - `user_page_t` now tracks only:
    - `va`
    - `pa`
    - `writable_intent` (software policy intent used for COW semantics)
    - `cow`
  - removed duplicated soft fields for current hardware mapping attributes.
- Added strict hardware mapping validation path:
  - `task_page_validate_hw()` reads live L3 PTE bits and validates:
    - descriptor validity/type
    - PA matches tracked page backing
    - non-writable intent pages are not mapped writable
    - COW pages are not mapped writable
- Updated COW decision points to validate against actual mapping before acting:
  - `fork()` now rejects inconsistent writable/COW states based on live PTE bits.
  - COW fault resolution now requires hardware read-only mapping on COW page and uses live executable bit when remapping writable.
- Kept per-task ASIDs and ASID-aware single-page invalidations:
  - `mmu_tlb_flush_va(va, asid)` uses `tlbi vae1is` operand with ASID+VA.
  - COW remaps, demand-map inserts, and unmaps flush only impacted VA in the owning address space.
- Stability note:
  - Attempted no-global-flush TTBR0 context switch path (`msr ttbr0_el1` + `isb`) regressed with early EL0 unknown exceptions.
  - Restored conservative global flush on TTBR0 context switch for runtime stability while retaining ASID-scoped `vae1is` page-level invalidation path.
- Re-ran full regression (`make`, `test.sh`) after cleanup; PASS.

### Step 19: PTE-native COW Tag + ASID Epoch/Fast TTBR0 Switch
- Migrated COW state source-of-truth fully into hardware PTE metadata:
  - added software-defined PTE tag `PTE_SW_COW` (bit 55)
  - removed duplicated `cow` field from `user_page_t`
  - COW checks now derive from live page descriptor bits:
    - writable bit (`AP`)
    - execute bit (`UXN`)
    - software COW tag
- Updated COW fork/write-fault flow:
  - `fork()` maps writable-intent pages as read-only + `PTE_SW_COW` in both parent/child
  - COW write fault accepts only `RO + COW` mappings, then resolves to writable non-COW mapping
  - read-only intent mappings are rejected if they carry COW metadata
- Added full-ASID TLB invalidate helper:
  - `mmu_tlb_flush_asid(asid)` via `tlbi aside1is`
  - full user address-space rewrite (`task_unmap_user`, used by `exec`) now invalidates that ASID to prevent stale translations
- Implemented ASID lifecycle with rollover epoch:
  - introduced `asid_epoch` in scheduler/task state
  - ASID allocator now uses bounded hardware-safe range (`1..255`)
  - on exhaustion:
    - global TLB invalidate
    - epoch increment
    - all tasks marked stale epoch
    - current task immediately retagged with fresh ASID
  - tasks lazily retag to current epoch on schedule (`task_ensure_asid`)
- Enabled fast TTBR0 context-switch path:
  - `mmu_switch_ttbr0()` now writes TTBR0 + `isb` only (no global flush per switch)
  - correctness maintained by:
    - ASID-scoped page invalidations (`vae1is`)
    - ASID-wide invalidation on full address-space rewrite (`aside1is`)
    - epoch rollover invalidation policy
- Validation:
  - standard regression: `make`, `test.sh` PASS
  - additional stress: 320+ fork/exec shell command loop crossing ASID rollover, then `cowtest` and `vmtest` PASS.

### Step 20: Hard COW Test Matrix + CI-style Gates
- Added new EL0 regression utility `/bin/cowmatrix` covering hard COW scenarios:
  - nested forks (grandchild writes, child/parent must remain isolated)
  - multi-page writes across 3+ pages under COW pressure
  - `exec` after `fork` with parent-memory invariants
  - `brk/sbrk` grow+shrink interactions after `fork`
  - kernel `copy_to_user` writes into COW pages (`read()` into inherited COW buffer)
  - parent-exit-before-child-write orchestration with pipe confirmation
- Extended automated smoke gates (`test.sh`):
  - executes `cowmatrix`
  - requires per-subtest PASS markers plus overall PASS marker
  - fails on any `[cowmatrix] FAIL`
- Added orphan-zombie background reaping for robustness:
  - scheduler reaps zombie tasks whose parent is no longer alive
  - prevents task-slot leakage in detached/orphan test paths.
- Re-ran full regression (`make`, `test.sh`) in QEMU nographic; PASS.

### Step 21: Interactive `cat` UX + Test Utility Unship
- Updated `/bin/cat` behavior to avoid apparent hang on interactive no-arg usage:
  - `cat` without args now prints usage and exits.
  - explicit stdin mode remains available via `cat -`.
  - supports multiple operands (`cat file1 file2 ...` and mixed `-`).
- Removed CoW stress utilities from shipped `/bin` image and default smoke gates:
  - unshipped: `/bin/cowtest`, `/bin/vmtest`, `/bin/cowmatrix`
  - removed corresponding checks from `test.sh`.
- Test utilities were unshipped from `/bin`; sources were later removed in Step 22 cleanup.

### Step 22: Process Model Hardening + Scale/OOM Guards
- Hardened process lifecycle semantics:
  - added explicit `kill` syscall path (`SYS_KILL`) and EL0 wrapper (`sys_kill`) with `/bin/kill` utility.
  - `wait(pid, status)` now enforces strict waitpid-style filtering (`pid==-1` any-child, `pid>0` exact child, reject unsupported `pid<=-2`/`pid==0`).
  - wakeup path for child exit now validates parent/child relation before waking waiters.
- Implemented reparent-to-init behavior:
  - exiting task reparents all live children to init.
  - zombie children reparented to init trigger waiter wakeups so init can reap.
  - orphan-zombie auto-reap now runs only as fallback when no live init exists.
- Added process-scale and memory-pressure controls:
  - raised `MAX_TASKS` from 16 to 64.
  - added PMM page accounting APIs (`pmm_used_pages`, `pmm_available_pages`) and refcount-backed ownership tracking.
  - added conservative fork admission guard using available-page headroom + COW reserve estimate.
  - added low-memory reserve checks for task setup, ELF mapping, demand paging, and COW copy allocation.
  - added per-task memory counters (`user_page_count`, `cow_page_count`, `private_page_count`, `mem_charge_pages`) refreshed from live mappings.
- Cleanup requested by user:
  - removed historical COW test sources from tree: `user/cowtest.c`, `user/vmtest.c`, `user/cowmatrix.c`.
  - kept test flow focused on runtime smoke and shipped `/bin` commands.

### Step 23: Process Control + Nonblocking I/O Readiness
- Extended process control semantics:
  - `wait` path now supports waitpid-style options with `WNOHANG` and filter forms (`pid>0`, `-1`, `0`, `<-1` for process-group waits).
  - added process-group metadata (`pgid`) to tasks and new syscalls: `setpgid`, `getpgid`.
  - upgraded `kill` to `kill(pid, sig)` with basic signal model (`sig=0`, `SIGTERM`, `SIGKILL`) and process-group targeting:
    - single pid (`pid>0`)
    - caller group (`pid==0`)
    - specific group (`pid<-1`)
    - broad target set (`pid==-1`, excluding init by policy).
  - exit path sends basic `SIGCHLD` pending notification bit to parent and keeps reparent-to-init + zombie wake behavior.
- Added nonblocking/readiness syscalls:
  - new `fcntl` syscall with `F_GETFL`/`F_SETFL`.
  - `O_NONBLOCK` support in open flags and descriptor mode updates.
  - pipe `read`/`write` now return `-11` (EAGAIN-style) on would-block for nonblocking FDs.
  - added `poll` syscall (`fu_pollfd_t`) with `POLLIN/POLLOUT/POLLERR/POLLHUP/POLLNVAL` readiness on stdio/inode/pipe FDs.
- Added UART RX readiness primitive (`uart_rx_ready`) to support nonblocking stdin readiness checks.
- Validation:
  - full build and `test.sh` pass in QEMU nographic.
  - added smoke checks for `/bin/kill` failure-path behavior (protect init / missing pid) in `test.sh`.

### Step 24: `mmap` / `munmap` / `mprotect` with Lazy Demand Faults
- Added VM syscalls and ABI entries:
  - `mmap(addr, len, prot, flags, fd, offset)`
  - `munmap(addr, len)`
  - `mprotect(addr, len, prot)`
- Implemented anonymous private VMA tracking per task (`MAX_VMAS`):
  - supports `MAP_PRIVATE|MAP_ANONYMOUS` mappings
  - supports non-overwriting `MAP_FIXED` placement
  - non-fixed mappings use top-down gap search between `heap_end` and stack floor
  - `heap_limit` is now recomputed against active VMAs to prevent `brk` overlap.
- Added lazy population on translation fault:
  - first read/write/exec access to unmapped VMA page allocates/map page on demand
  - protection checks are enforced from VMA `prot` bits before mapping.
- Added `munmap` range teardown:
  - VMA boundary splitting + exact-range VMA removal
  - mapped pages in range are unmapped and freed
  - TLB invalidation via existing per-page flush path.
- Added `mprotect` range updates:
  - VMA boundary splitting + per-range `prot` rewrite
  - mapped pages are remapped with updated permissions
  - write-enable on shared mapped pages performs private copy before granting write to preserve isolation.
- Validation:
  - temporary EL0 runtime probe (`mmtest`) passed (`[mmtest] PASS`) in QEMU nographic
  - temporary test binary/file removed after validation (not shipped in `/bin`)
  - final `make` + `test.sh` pass.

### Step 25: File-backed `mmap`, `MAP_SHARED`, and `MAP_FIXED` Overwrite
- Extended VMA model to support file-backed mappings:
  - VMA now tracks `inode` backing pointer and per-region `file_offset`.
  - `mmap` now accepts `fd != -1` when `MAP_ANONYMOUS` is not set.
  - page-aligned `offset` enforcement for file mappings.
- Added `MAP_SHARED` flag support to syscall ABI constants.
- Implemented fault-time file page population:
  - translation faults on file-backed VMAs allocate a page and copy file bytes from `(file_offset + page_delta)`.
  - remaining bytes are zero-filled by allocator defaults.
- Implemented shared writeback path:
  - unmapping/release of pages in `MAP_SHARED` file-backed VMAs writes back page contents to the inode buffer.
  - file size is updated when writeback extends logical EOF inside capacity.
- Implemented real overwrite behavior for `MAP_FIXED`:
  - overlapping VMAs are now unmapped/replaced in target range instead of hard failing.
  - overlap handling supports full-remove, head/tail trim, and interior split cases.
- Updated fork behavior for `MAP_SHARED` pages:
  - shared mappings are inherited without forcing COW remap.
  - private mappings keep existing COW fork behavior.
- Hardened build reliability:
  - added compiler dependency generation (`-MMD -MP`) and `.d` inclusion in `Makefile`.
  - prevents stale object/header-layout mismatch regressions (observed once after `task_t`/VMA header changes).
- Validation:
  - full clean rebuild and smoke regression (`make clean && make`, `./test.sh`) pass.
  - temporary EL0 runtime validator (`/bin/mmmaptest`) passed file-backed/shared/fixed checks, then was removed from shipped `/bin` image per request.

### Step 26: Global File Page Cache for `MAP_SHARED` Coherence
- Added kernel-global file page cache (`kernel/pagecache.c`, `include/pagecache.h`):
  - cache key: `(inode*, file_off_page_aligned)`
  - cache value: physical page backing shared file data
  - bounded cache size (`MAX_FILE_CACHE_PAGES`) and PMM-refcount integration.
- Integrated cache lifecycle:
  - kernel init now calls `pagecache_init()`.
  - `MAP_SHARED` file-backed translation faults now map cached physical pages (create-on-miss, reuse-on-hit).
  - independent faults in different mappings/tasks now converge on same physical page.
- Kept `MAP_PRIVATE` file-backed mappings isolated:
  - private file mappings still allocate/copy per-fault private pages.
- Updated shared writeback path:
  - `MAP_SHARED` unmap/release now writes mapped page bytes back through pagecache-aware writeback helper.
  - partial tail-page writeback respects VMA end (no accidental full-page file extension beyond mapped tail).
- Validation:
  - full build + smoke regression (`make`, `./test.sh`) pass.
  - temporary EL0 runtime validator (`/bin/mmcachetest`) verified:
    - same-process independent-fault `MAP_SHARED` visibility,
    - cross-process/fork independent-fault visibility,
    - file writeback persistence after `munmap`.
  - temporary validator removed from shipped `/bin` after pass.

### Step 27: Virtio-blk Transport + Block Cache Foundation
- Added virtio block transport driver:
  - new files: `include/virtio_blk.h`, `kernel/virtio_blk.c`
  - supports virtio-mmio transport version 1 (legacy) and version 2 (modern).
  - scans virtio-mmio slots (`VIRTIO_MMIO0_BASE + n*VIRTIO_MMIO_STRIDE`) to find a block device instead of assuming slot 0.
  - queue setup for one request queue with descriptor chain polling path (`header`, `data`, `status`) and synchronous completion handling.
  - exposes kernel API:
    - `virtio_blk_ready()`
    - `virtio_blk_capacity_sectors()`
    - `virtio_blk_block_size()`
    - `virtio_blk_rw_sector(...)`
- Added kernel block cache layer:
  - new files: `include/block_cache.h`, `kernel/block_cache.c`
  - fixed-size LRU sector cache (`BLOCK_CACHE_LINES`) over 512-byte sectors.
  - dirty tracking and explicit flush (`block_cache_flush`).
  - read/write APIs:
    - `block_cache_read(lba, buf, count)`
    - `block_cache_write(lba, buf, count)`
- MMIO mapping updates for virtio-mmio region:
  - mapped `0x0A000000` 2MiB device block in:
    - kernel translation tables (`kernel/mmu.c`)
    - per-task TTBR0 templates (`kernel/task.c`)
- Boot integration:
  - kernel now initializes `virtio_blk` then `block_cache`.
  - graceful fallback remains if no block device is found.
- Build/run integration:
  - `run.sh` now auto-creates/uses `build/disk.img` and attaches it via `virtio-blk-device`.
  - `test.sh` now creates a temporary raw disk image and boots QEMU with virtio-blk attached.
- Validation:
  - full build + smoke (`make`, `./test.sh`) pass.
  - boot log confirms runtime attach:
    - `[virtio-blk] ready ...`
    - `[block-cache] ready ...`
  - no temporary EL0 test binaries were left in shipped `/bin`.

### Step 28: Virtio-blk Hardening (IRQ Completion, Non-512 Blocks, Recovery, Stress)
- Upgraded GIC IRQ provisioning to generic per-INTID enable path:
  - added `gic_enable_irq(intid, priority)` with correct IGROUP/ISENABLER/ICPENDR programming and byte-granular priority/target programming.
  - `gic_init()` now enables timer IRQ through this common path.
- Added virtio IRQ routing in trap path:
  - `trap_handle_irq` now dispatches non-timer IRQs through `virtio_blk_handle_irq(intid)`.
  - virtio MMIO interrupt status is ACKed on IRQ path.
- Hardened virtio-blk request engine:
  - retained single-queue model but moved completion handling to IRQ-aware path with bounded fallback polling (`yield` + timeout) to avoid deadlock.
  - added I/O timeout detection and recovery path: failed I/O triggers device re-init and single retry.
  - added `virtio_blk_flush()` support (virtio `VIRTIO_BLK_T_FLUSH`) when negotiated.
  - added host-feature awareness logging for multi-queue capability (host `MQ` noted; driver remains single-queue by design).
- Added non-512 logical block support:
  - removed hard 512-byte enforcement in block-cache init.
  - `virtio_blk_rw_sector()` now supports devices with logical block size that is a multiple of 512 via aligned bounce-buffer read/modify/write.
  - validated with QEMU `logical_block_size=4096,physical_block_size=4096` boot (`sector_bytes=4096`, selftest pass).
- Hardened block-cache durability path:
  - `block_cache_flush()` now calls `virtio_blk_flush()` after dirty line writeback.
- Added stress/fault-injection validation without shipping test binaries in `/bin`:
  - kernel boot-time block-cache selftest performs repeated randomized sector writes/reads, explicit flush, and out-of-range fault checks.
  - expanded `test.sh` with heavier interactive I/O sequence and assertions for selftest + stress output.
- Validation:
  - `make -j4` clean.
  - `./test.sh` pass with extended stress matrix.
  - additional QEMU run with 4KiB logical block device confirms non-512 path works and kernel remains interactive.

### Step 29: ext4 Read-only Mount + Lookup + Read
- Added ext4 read-only backend:
  - new files: `include/ext4.h`, `kernel/ext4.c`
  - mount path is `/ext` (mounted from virtio block disk during `fs_init`).
- Implemented ext4 mount parsing (read-only):
  - superblock validation (`s_magic`, block size, inode size, group geometry).
  - feature gating for unsupported modes (rejects unsupported `INCOMPAT` and `RO_COMPAT` combinations, including `BIGALLOC` and `META_BG`).
  - group descriptor parsing with optional 64-bit inode-table block handling.
- Implemented ext4 inode load + data path:
  - inode metadata load from inode table.
  - file data reads via:
    - extents tree mapping (`EXT4_EXTENTS_FL`) with leaf and index traversal.
    - fallback direct + single-indirect block mapping for non-extent inodes.
  - sparse/hole reads return zeroed bytes.
- Implemented ext4 directory semantics:
  - iterative parse of ext4 dirent records (`rec_len`, `name_len`, optional `file_type`).
  - `.` and `..` filtered from userspace `readdir` stream.
  - path lookup by name inside ext4 directories.
- Integrated ext4 backend into VFS:
  - added inode backend tag (`FS_KIND_MEM` / `FS_KIND_EXT4`) and ext4 inode id field.
  - `fs_lookup` now dispatches child lookup to ext4 when traversing ext-backed directories.
  - `fs_read` dispatches to ext4 backend for ext-backed inodes.
  - create/rename/unlink/write operations are blocked on ext-backed paths (strict read-only enforcement).
- Boot/init ordering fix:
  - moved `fs_init()` after virtio/block-cache init in `kernel_main` so ext4 mount runs after block device is ready.
- Host runtime + test integration:
  - `run.sh` now seeds `build/disk.img` as ext4 (when `mke2fs` is available) with sample files under `/ext`.
  - `test.sh` now builds a seeded ext4 image and validates:
    - `ls /ext`
    - `cat /ext/hello.txt`
    - `ls /ext/docs`
    - `cat /ext/docs/readme.txt`
- Validation:
  - `make -j4` pass.
  - full `./test.sh` pass in QEMU nographic with ext4 mount log:
    - `[ext4] mounted ro at /ext ...`
  - no new EL0 `/bin` test binaries were introduced.

### Step 30: ext4 RO Completion Hardening
- Completed ext4 read-only data mapping coverage:
  - non-extent inode block maps now support:
    - direct
    - single indirect
    - double indirect
    - triple indirect
  - extent-header bounds checks added before parsing entries.
- Improved mount geometry correctness:
  - group count now uses `s_first_data_block` when deriving data-group span.
  - inode and group descriptor size checks now scale with ext4 block size (up to 4KiB in this implementation).
- Added symlink-aware lookup behavior in ext4 backend:
  - supports fast symlink target extraction from inode body.
  - supports regular symlink target reads from data blocks.
  - resolves symlink targets through normalized absolute paths with bounded recursion depth.
- Expanded ext4 read-only test coverage:
  - seeded ext4 images now include `/ext/readme-link -> docs/readme.txt`.
  - `test.sh` validates symlink-follow read path (`cat /ext/readme-link`).
  - extra non-extent ext4 image validation run (1KiB blocks, `^extent`) confirmed mount + lookup + file read markers end-to-end.
- Validation:
  - `make -j4` pass.
  - `./test.sh` pass with ext4 mount + directory + file + symlink checks.
  - additional QEMU non-extent ext4 run passed (`BEGIN-LEGACY` to `END-MARKER-42` file read markers).

### Step 31: ext4 Write Path (Allocation/Bitmaps + Metadata Updates)
- Added ext4 mutation APIs:
  - `ext4_create_file(parent, name)` and `ext4_write(inode, off, buf, len)`.
  - wired into VFS:
    - `fs_create_file()` now delegates to ext4 backend when parent is ext-backed.
    - `fs_write()` now delegates to ext4 backend for ext-backed files.
- Implemented on-disk allocation and metadata updates:
  - inode allocation via inode bitmap scan + set.
  - data block allocation via block bitmap scan + set.
  - group descriptor free-count updates (`bg_free_blocks_count`, `bg_free_inodes_count`).
  - superblock free-count updates (`s_free_blocks_count`, `s_free_inodes_count`).
  - inode table writeback for mode/size/flags/blocks/pointers.
  - directory entry insertion into existing ext4 directory blocks (record split / free-slot reuse).
- Implemented writable data mapping path:
  - non-extent inode writes with block mapping update support:
    - direct
    - single indirect
    - double indirect
    - triple indirect
  - empty extent-flagged regular files are converted to non-extent form for write path.
  - existing non-empty extent files remain read-only in this phase.
- Added write-safety gating at mount:
  - mount reports `rw` only when fs lacks features this implementation cannot safely update.
  - write path is disabled when journal or metadata/group checksums are present (`HAS_JOURNAL`, `GDT_CSUM`, `METADATA_CSUM`).
- Durability behavior:
  - metadata/data updates use block-cache writeback path and explicit flush on ext4 create/write operations.
- Kernel robustness fix uncovered during ext4 write stress:
  - increased per-task kernel stack size from 16KiB to 64KiB to avoid deep ext4-path stack exhaustion under syscall nesting.
- Validation:
  - full `make -j4` + `./test.sh` pass.
  - `test.sh` now verifies ext4 write/append readback:
    - `echo ext4-write > /ext/new.txt`
    - `echo ext4-append >> /ext/new.txt`
    - `cat /ext/new.txt`
  - additional non-extent ext4 image run (`-O ^extent`) validated:
    - large legacy file read markers,
    - new file create/write/read on ext4.

### Step 32: ext4 Free-path Mutations (`unlink`/`rmdir`/`rename`/`truncate`)
- Completed ext4 mutation entry points and VFS wiring:
  - `ext4_unlink(inode)`
  - `ext4_rmdir(inode)`
  - `ext4_rename(inode, new_parent, new_name)`
  - `ext4_truncate(inode, new_size)`
- Added free-path block/inode release behavior on ext4:
  - file unlink decrements link count and frees data blocks + inode bitmap on last link.
  - empty directory removal frees directory data blocks + inode bitmap and updates parent link count.
  - truncate shrink path frees tail blocks; truncate-to-zero fully releases file blocks.
- Added non-extent pointer teardown helper for truncate shrink:
  - clears direct/single/double/triple mappings and prunes empty indirect metadata blocks.
- Added rename mutation support:
  - directory entry remove/add across ext4 directories.
  - directory move updates child `..` inode reference.
  - parent directory link count adjustments for cross-parent directory moves.
- Added cache invalidation on inode free to avoid stale ext4 inode reuse in cache slots.
- Kept non-empty extent-file mutability path safe:
  - truncate/write on non-empty extent files continues through existing conversion-to-non-extent flow with extent metadata-node cleanup, then updates block maps via mutable direct/indirect path.
- Updated userland behavior to exercise truncate syscall path:
  - shell `>` now uses `O_TRUNC` (instead of unlink+create).
  - `/bin/cp` destination open now uses `O_TRUNC`.
- Expanded regression (`test.sh`) ext4 matrix:
  - truncate overwrite on existing ext4 file (`>`),
  - rename (`mv`) on ext4 file,
  - unlink (`rm`) on ext4 file,
  - empty directory removal (`rmdir /ext/empty`).
- Validation:
  - `make -j4` PASS.
  - `./test.sh` PASS in QEMU nographic with ext4 mutation sequence succeeding end-to-end.

### Step 33: Native In-place Extent-tree Mutation (No Non-empty Extent Fallback Conversion)
- Implemented extent-native mutation engine in `kernel/ext4.c`:
  - recursive extent tree run collector (supports indexed extent trees),
  - run merge/validation,
  - extent-tree rebuild/commit path that writes new extent metadata nodes and retires old metadata nodes.
- Added native extent allocation/update path for writes:
  - `ext4_write()` now keeps extent format for extent inodes and inserts newly allocated blocks into extent maps directly.
  - removed non-empty extent fallback conversion from write path.
- Added native extent truncate-shrink path:
  - `ext4_truncate()` now trims extent runs in-place for extent inodes, frees dropped data blocks, rebuilds extent tree, and preserves extent format.
  - partial tail block is still zeroed after shrink.
- Kept pointer-map path for non-extent inodes unchanged.
- Added robustness fixes while touching write path:
  - fresh newly allocated blocks are zero-filled for partial-block writes (avoids stale on-disk data exposure from read-modify-write on uninitialized new block).
  - allocation failure paths now free just-allocated blocks on mapping-insert/set failure to avoid bitmap leaks.
- Safety behavior:
  - unwritten extents (`ee_len` high-bit) are rejected for mutation in this implementation (safe fail, no silent corruption path).
- Validation:
  - `make -j4` PASS.
  - full `./test.sh` PASS in QEMU nographic with explicit mutation of an existing seeded extent file (`/ext/hello.txt`) plus ext4 write/append/truncate/rename/unlink/rmdir sequence.

### Step 34: `devfs` + VFS Mount Table + `mount/umount` Syscalls
- Added in-memory `devfs` nodes under `/dev`:
  - `/dev/null`
  - `/dev/zero`
  - `/dev/tty`
  - `/dev/vda`
- Added device I/O handling in VFS read/write paths:
  - `/dev/null`: read EOF, write sink.
  - `/dev/zero`: read zero bytes, write sink.
  - `/dev/tty`: UART-backed read/write.
  - `/dev/vda`: byte-addressed block-device I/O over block-cache sector RMW.
- Added VFS mount table management (`MOUNT_TABLE_MAX`) with mountpoint tracking.
- Added kernel mount APIs:
  - `fs_mount(source, target, fstype, flags)`
  - `fs_umount(target, flags)`
- Added ext4 unmount support:
  - new `ext4_unmount(mountpoint)` path to detach and restore mountpoint inode back to memfs role.
- Added new syscalls and userspace wrappers:
  - `SYS_MOUNT`
  - `SYS_UMOUNT`
  - wrappers: `sys_mount`, `sys_umount`
- Added EL0 admin utilities:
  - `/bin/mount`
  - `/bin/umount`
- Kept boot auto-mount behavior by routing initial mount through the new generic mount path (`/dev/vda` -> `/ext`).
- Added mountpoint safety checks in VFS mutations:
  - active mountpoints cannot be removed or renamed.
- Validation:
  - `make -j4` PASS.
  - `./test.sh` PASS with new checks for:
    - `/dev` listing and `/dev/null` + `/dev/tty` usage,
    - explicit `umount /ext` + `mount /dev/vda /ext ext4` flow,
    - post-remount `/ext` listing integrity.

### Step 35: Default Mountpoint Cleanup (`/mnt`) + ext4 `mkdir`
- Removed default `/ext` directory from root FS layout.
- Default boot auto-mount now targets `/mnt`:
  - `/dev/vda` is mounted to `/mnt` during `fs_init()` through `fs_mount(...)`.
- Fixed ext4 mount/unmount logging to print actual mountpoint path:
  - no more hardcoded `/ext` in mount logs.
- Implemented ext4 directory creation path:
  - added `ext4_create_dir(parent, name)` and VFS dispatch from `fs_create_dir`.
  - allocates new directory inode + one data block.
  - initializes on-disk `.` and `..` dirents.
  - inserts new entry into parent directory.
  - updates parent link count and persists inode metadata.
- Verified ext4 directory mutation flow works on mounted ext4:
  - `mkdir /mnt/hi`
  - `touch /mnt/hi/file1`
  - `ls /mnt/hi`
  - `rm /mnt/hi/file1`
  - `rmdir /mnt/hi`
- Validation:
  - `make -j4` PASS.
  - full `./test.sh` PASS updated for `/mnt` path and ext4 mkdir/rmdir lifecycle.

## Current Status
- Kernel boots in EL2 and drops to EL1.
- EL0 user programs run with SVC syscalls.
- Timer IRQ preemption is active via generic timer + GICv2.
- Per-task EL1 kernel stacks are active and SP switches with task context switch.
- Real kernel `pipe()` syscalls are active with blocking wakeup semantics.
- Syscall dispatch uses a centralized metadata table with compile-time enum/table consistency checks.
- Tick-based `sleep` syscall is active with task blocking/wakeup on timer IRQs.
- COW `fork` is active with write-fault resolution and PMM refcounted shared pages.
- Process model now supports `wait`/`waitpid` semantics (`WNOHANG` + process-group wait filters), `kill(sig)` with process-group targeting, reparent-to-init, and parent-validated zombie wakeups.
- Nonblocking descriptor controls (`fcntl` + `O_NONBLOCK`) and `poll` readiness checks are active.
- Task scale has been raised to 64 slots with conservative fork/OOM admission checks and PMM-backed page availability accounting.
- Demand paging is active for EL0 translation faults with explicit `brk` heap-end gating.
- Anonymous and file-backed `mmap` regions are supported with lazy fault-time page population.
- `MAP_SHARED` is supported with inode-backed writeback on unmap/teardown.
- Global file page cache is active for `MAP_SHARED` file-backed mappings, giving immediate coherence across independently faulted mappings.
- Virtio-mmio block transport is active under QEMU `virt` with attached `virtio-blk-device`.
- Kernel LRU block cache layer is active and ready for upcoming on-disk filesystem integration.
- Virtio IRQ completion path is active with timeout+reinit retry recovery and negotiated flush support.
- Logical block sizes greater than 512 bytes (multiple-of-512) are supported through 512-byte compatibility RMW in the driver.
- Boot-time block-cache stress/fault selftest is active and validated in QEMU nographic.
- ext4 read-only mount/lookup/readdir/read is active from virtio disk (default mountpoint now `/mnt`).
- ext4 RO lookup follows symlinks with bounded recursion and normalized target paths.
- ext4 create/write path is active for compatible no-journal/no-metadata-checksum filesystems, including block/inode bitmap allocation and superblock/group descriptor free-count updates.
- ext4 unlink/rmdir/rename/truncate free-path mutations are active, including block/inode release and parent link-count updates for directory moves/removals.
- extent-format files are now mutated natively in extent trees for write allocation and truncate shrink (no non-empty extent conversion fallback in normal path).
- `/dev` device nodes are present (`null`, `zero`, `tty`, `vda`) and usable from EL0 through VFS.
- mount table tracking is active with `mount/umount` syscalls and EL0 `/bin/mount` + `/bin/umount` tools.
- default ext4 mountpoint is now `/mnt` (no default `/ext` directory).
- ext4 `mkdir` is active for mounted ext4 directories.
- `munmap` and `mprotect` are supported over mapped VMA ranges.
- Stack growth uses strict guard-page + SP-based validation (no arbitrary stack-range mapping).
- COW/fault decisions are validated against live hardware PTE bits rather than duplicated soft mapping state.
- COW state is encoded in PTE software-defined bits (`PTE_SW_COW`) and enforced from live descriptors.
- Single-page remaps use ASID-scoped targeted TLB invalidation (`tlbi vae1is`).
- TTBR0 context switch runs with fast ASID-tagged switch (no per-switch global TLB flush).
- Full address-space rewrites (`exec`) use ASID-wide invalidation (`tlbi aside1is`).
- ASID rollover uses epoch invalidation + lazy task retagging.
- VFS and shell command execution function in QEMU nographic mode.
- Shell supports quoting/escaping, redirection (`<`/`>`/`>>`), concurrent real pipelines (`|`), `cd`, `fork/exec/wait`, and automatic respawn from init.
- `/bin/clear`, `/bin/mkdir`, `/bin/rmdir`, `/bin/rm`, `/bin/pwd`, `/bin/touch`, `/bin/cp`, `/bin/mv`, `/bin/sleep`, and `/bin/kill` execute in EL0 and are integrated into VFS/syscall stack.

## Improvements Next
- Add dedicated regression utility for negative VM cases (out-of-break heap touch and non-guard stack touch should fault/exit).
- Read ASID width from `ID_AA64MMFR0_EL1` and size allocator dynamically (8-bit vs 16-bit ASID hardware).
- Add optional per-ASID flush generation bookkeeping to minimize rollover stall cost under heavy churn.
- Add ELF `PT_INTERP`/dynamic support (currently static ET_EXEC only).
- Extend reclaim from user pages into generalized kernel object reclaim (inode/file buffers, metadata).
- Expand VFS with mount points and device nodes.
- Extend virtio-blk from single queue to true multi-queue submission/completion paths.
- Add support for unwritten extent mutation/initialization transitions (`ee_len` high-bit) instead of current safe-fail behavior.
- Add ext4 `link/symlink` creation syscalls/userland (`ln`, `ln -s`) for fuller POSIX directory semantics.
- Add mount busy/reference checks (open-FD pinning) before `umount` to prevent detaching filesystems with active users.
- Extend mount layer beyond single ext4 instance (current ext4 backend state is global/single-mount).
- Add journal/recovery-aware write path before enabling writes on journaled ext4 images.
- Add checksum/encryption/casefold-aware ext4 directory handling for broader feature coverage.
- Add regression test matrix for syscalls and trap behavior.

## Web References Used
- ARM exception level and VBAR behavior: https://developer.arm.com/documentation/102412/latest/
- Linux AArch64 boot protocol (entry state context): https://www.kernel.org/doc/html/latest/arch/arm64/booting.html
- QEMU `virt` machine docs: https://www.qemu.org/docs/master/system/arm/virt.html
- QEMU ARM source memory map reference (`virt.c`): https://gitlab.com/qemu-project/qemu/-/blob/master/hw/arm/virt.c
- QEMU invocation semantics (`-nographic`, serial/monitor multiplexing): https://qemu-project.gitlab.io/qemu/system/invocation.html
- QEMU `virt` memory map (`VIRT_GIC_DIST`/`VIRT_GIC_CPU`): https://sources.debian.org/src/qemu/2.1%2Bdfsg-5~bpo70%2B1/hw/arm/virt.c/
- QEMU `virt` timer IRQ wiring (`ARCH_TIMER_NS_EL1_IRQ` -> PPI 14): https://patchwork.ozlabs.org/patch/183350/
- POSIX utility specs for command semantics:
  - `rm`: https://pubs.opengroup.org/onlinepubs/9699919799/utilities/rm.html
  - `rmdir`: https://pubs.opengroup.org/onlinepubs/9699919799/utilities/rmdir.html
  - `mkdir`: https://pubs.opengroup.org/onlinepubs/9699919799/utilities/mkdir.html
- POSIX shell grammar, quoting, redirection, and pipeline semantics:
  - https://pubs.opengroup.org/onlinepubs/9799919799/utilities/V3_chap02.html
- POSIX `pipe()` function definition:
  - https://pubs.opengroup.org/onlinepubs/9699919799/functions/pipe.html
- Linux `pipe(2)` behavior reference:
  - https://man7.org/linux/man-pages/man2/pipe.2.html
- Linux `syscall(2)` behavior reference:
  - https://man7.org/linux/man-pages/man2/syscall.2.html
- Linux arm64 syscall implementation reference:
  - https://github.com/torvalds/linux/blob/master/arch/arm64/kernel/syscall.c
- Linux arm64 ESR exception class and ISS bit definitions:
  - https://codebrowser.dev/linux/linux/arch/arm64/include/asm/esr.h.html
- Linux arm64 TLB invalidation operand encoding (`__TLBI_VADDR`) and TLBI helpers:
  - https://codebrowser.dev/linux/linux/arch/arm64/include/asm/tlbflush.h.html
- Linux arm64 software PTE bit definitions (`PTE_SWBITS_MASK`, software attributes):
  - https://codebrowser.dev/linux/linux/arch/arm64/include/asm/pgtable-prot.h.html
- Linux arm64 ASID/context management reference:
  - https://codebrowser.dev/linux/linux/arch/arm64/mm/context.c.html
- Linux arm64 fault handling flow (`mm/fault.c`):
  - https://codebrowser.dev/linux/linux/arch/arm64/mm/fault.c.html
- Linux `brk(2)` userspace-visible semantics:
  - https://man7.org/linux/man-pages/man2/brk.2.html
- Linux `waitpid(2)` behavior reference:
  - https://man7.org/linux/man-pages/man2/waitpid.2.html
- Linux `kill(2)` behavior reference:
  - https://man7.org/linux/man-pages/man2/kill.2.html
- Linux `fcntl(2)` descriptor flag semantics (`F_GETFL`, `F_SETFL`, `O_NONBLOCK`):
  - https://man7.org/linux/man-pages/man2/fcntl.2.html
- Linux `poll(2)` readiness semantics (`POLLIN`, `POLLOUT`, `POLLHUP`, `POLLERR`):
  - https://man7.org/linux/man-pages/man2/poll.2.html
- Linux `setpgid(2)`/process-group control:
  - https://man7.org/linux/man-pages/man2/setpgid.2.html
- Linux `mmap(2)` API and flags:
  - https://man7.org/linux/man-pages/man2/mmap.2.html
- Virtio 1.2 specification (MMIO transport + block device):
  - https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html
- Linux virtio-mmio register offsets (uapi):
  - https://elixir.bootlin.com/linux/latest/source/include/uapi/linux/virtio_mmio.h
- Linux virtio block driver reference:
  - https://elixir.bootlin.com/linux/latest/source/drivers/block/virtio_blk.c
- QEMU ARM `virt` machine docs:
  - https://www.qemu.org/docs/master/system/arm/virt.html
- QEMU `virt` virtio-mmio base/stride/IRQ layout reference:
  - https://patchwork.ozlabs.org/comment/1930706/
- Linux page cache internals (`filemap.c`):
  - https://codebrowser.dev/linux/linux/mm/filemap.c.html
- Linux kernel page cache documentation:
  - https://www.kernel.org/doc/html/latest/mm/page_cache.html
- Linux ext4 name/inode operation flow (`unlink`, `rmdir`, `rename`) reference:
  - https://codebrowser.dev/linux/linux/fs/ext4/namei.c.html
- Linux ext4 extent operations reference:
  - https://codebrowser.dev/linux/linux/fs/ext4/extents.c.html
- Linux `truncate(2)` userspace-visible semantics:
  - https://man7.org/linux/man-pages/man2/truncate.2.html
- Linux `rename(2)` userspace-visible semantics:
  - https://man7.org/linux/man-pages/man2/rename.2.html
- Linux `unlink(2)` userspace-visible semantics:
  - https://man7.org/linux/man-pages/man2/unlink.2.html
- Linux `rmdir(2)` userspace-visible semantics:
  - https://man7.org/linux/man-pages/man2/rmdir.2.html
- Linux `mount(2)` userspace-visible semantics:
  - https://man7.org/linux/man-pages/man2/mount.2.html
- Linux `umount(2)` userspace-visible semantics:
  - https://man7.org/linux/man-pages/man2/umount.2.html
- Linux `/dev/null` and `/dev/zero` semantics:
  - https://man7.org/linux/man-pages/man4/null.4.html
- Linux `/dev/tty` semantics:
  - https://man7.org/linux/man-pages/man4/tty.4.html
- Linux `munmap(2)` behavior:
  - https://man7.org/linux/man-pages/man2/munmap.2.html
- Linux `mprotect(2)` semantics:
  - https://man7.org/linux/man-pages/man2/mprotect.2.html
- Linux `MAP_FIXED` replacement semantics (`mmap(2)`):
  - https://man7.org/linux/man-pages/man2/mmap.2.html
- Linux `sys_brk` implementation details (`mm/mmap.c`):
  - https://codebrowser.dev/linux/linux/mm/mmap.c.html
- xv6 COW fork lab reference model (design comparison only):
  - https://pdos.csail.mit.edu/6.1810/2022/labs/cow.html
- Linux ext4 superblock and feature flag definitions:
  - https://www.kernel.org/doc/html/latest/filesystems/ext4/super.html
- Linux ext4 inode and `i_block`/extent-related definitions:
  - https://www.kernel.org/doc/html/latest/filesystems/ext4/inodes.html
  - https://www.kernel.org/doc/html/latest/filesystems/ext4/ifork.html
- Linux ext4 directory entry format and file-type semantics:
  - https://www.kernel.org/doc/html/latest/filesystems/ext4/directory.html

### Step 36: ext4 links/stat + umount-busy protection
- Added new syscalls and userspace wrappers:
  - `link(2)`
  - `symlink(2)`
  - `readlink(2)`
  - `lstat(2)`
- Extended VFS API:
  - `fs_lookup_nofollow`
  - `fs_link`
  - `fs_symlink`
  - `fs_readlink`
  - `fs_lstat`
- Implemented ext4 operations:
  - hard-link create (`ext4_link`)
  - symlink create (`ext4_symlink`)
  - symlink target read (`ext4_readlink`)
  - metadata stat without following final symlink (`ext4_lstat`)
  - explicit no-follow child lookup (`ext4_lookup_child_nofollow`)
- Fixed path-final symlink semantics in VFS mutations:
  - `fs_unlink` now resolves target with no-follow (so unlink removes link itself, not referent)
  - `fs_rmdir` uses no-follow final lookup
  - `fs_rename` source and destination-existence checks now use no-follow final lookup
- Added umount busy protection:
  - new `task_mount_busy(mountpoint, mount_path)` scan across live tasks
  - denies `umount` when any task still references mount via:
    - current working directory under mount
    - open inode FDs under mount
    - mapped file-backed VMAs under mount
- Added EL0 `/bin/ln` utility:
  - `ln <src> <dst>` for hard links
  - `ln -s <target> <linkpath>` for symlinks
- Build/embed updates:
  - included `ln` in `USER_PROGS`
  - embedded `/bin/ln` in FS init image
- Test updates (`test.sh`):
  - ext4 hard-link and symlink command flow via `/bin/ln`
  - `umount` busy check (`cd /mnt; umount /mnt` must fail)
  - successful unmount/remount after leaving mountpoint
- Verification:
  - `make -j4` PASS
  - `./test.sh` PASS

### Step 37: Real AHCI/SATA block driver + dynamic `/dev` disk nodes
- Implemented a real AHCI driver (`kernel/ahci.c`) with:
  - PCIe ECAM scan on QEMU `virt` (`pci-host-ecam-generic` window)
  - AHCI controller discovery (class 0x01 / subclass 0x06)
  - BAR assignment for unmapped ABAR MMIO
  - per-port command engine setup (CLB/FB/command table)
  - ATA IDENTIFY, READ DMA EXT, WRITE DMA EXT, FLUSH CACHE EXT (polling path)
  - multi-disk exposure through AHCI ports
- Added MMU mappings for PCI windows required by AHCI:
  - low PCI MMIO BAR window mapping in L2 (`0x10000000..`)
  - high PCI ECAM mapping in L1 (`0x4010000000` region)
  - mirrored same mappings in per-task TTBR0 templates so EL1 syscall path remains stable after context switches
- Added AHCI public API (`include/ahci.h`) and kernel init hook (`kernel/kernel.c`).
- Extended `dev_kind_t` to support SATA disks:
  - `DEV_SDA..DEV_SDH`
- Device-node presence is now dynamic and hardware-backed:
  - `/dev/vda` only created when virtio-blk is present/ready
  - `/dev/sda`, `/dev/sdb`, ... created only for discovered AHCI disks
  - if device/controller is absent, corresponding node is not created
- Refactored block cache to selectable backend (`kernel/block_cache.c`):
  - `block_cache_attach(dev_kind)` chooses active mounted block backend (virtio or AHCI)
  - cache now flushes/reads/writes via backend abstraction instead of hardcoding virtio
  - kept cache selftest non-destructive
- Updated VFS/device data path (`kernel/fs.c`):
  - raw `/dev/vd*` and `/dev/sd*` read/write dispatch to respective backend driver directly
  - `fs_mount` now accepts any active block dev (`/dev/vda`, `/dev/sda`, `/dev/sdb`, ...)
  - mount path attaches block cache to selected source device before ext4 mount
  - boot auto-mount prefers `/dev/vda`, falls back to `/dev/sda` when virtio is absent
- Updated `run.sh`:
  - supports optional AHCI disk attach via env vars:
    - `AHCI_DISK_PATH`
    - `AHCI_DISK2_PATH`
  - supports `DISABLE_VIRTIO=1` for SATA-only boot tests
- Extended `test.sh` with AHCI-only validation run:
  - boots with two AHCI disks and no virtio disk
  - verifies `/dev/sda` and `/dev/sdb` exist
  - verifies `/dev/vda` does not exist when virtio not attached
- Verification:
  - `make -j4` PASS
  - `./test.sh` PASS (existing virtio/ext4 flow + AHCI-only flow)

### Step 38: AHCI IRQ completion + recovery + PRDT multi-sector + partition nodes
- Upgraded AHCI completion path from pure polling to IRQ-aware completion:
  - enabled AHCI interrupts (`GHC.IE`) and per-port interrupt masks.
  - routed PCI INTx to GIC (`gic_enable_irq(...)`) using PCI config line/pin fallback swizzle.
  - added `ahci_handle_irq(intid)` and hooked it in EL1 IRQ dispatch (`trap.c`).
  - command wait loop now uses IRQ event progress (`yield` while waiting) with bounded timeout fallback.
- Added robust AHCI recovery flows:
  - per-port command engine rebase/restart path.
  - COMRESET sequence (`PxSCTL.DET`) for failed ports.
  - HBA-level reset fallback (`GHC.HR`) with rebase of known ports.
  - command retry-once path for READ/WRITE/FLUSH/IDENTIFY failures.
- Added multi-sector AHCI command path:
  - implemented `ahci_rw(...)` for `count > 1`.
  - PRDT now supports multiple entries (`AHCI_PRDT_MAX=32`) with up to 4 MiB per entry.
  - VFS block-device fast path now uses multi-sector transfers for aligned full-sector reads/writes.
- Finished partition support integration:
  - GPT + MBR probing remains in `fs.c` and now validated end-to-end with AHCI.
  - partition device nodes exposed as `/dev/sdX1..`.
  - auto-mount order now prefers partition nodes first (`/dev/vda1`, `/dev/sda1`) then whole-disk fallbacks.
  - block cache attach remains inode-based (`block_cache_attach_inode`) so mount-on-partition uses correct LBA base/length window.
- Reduced test runtime (previously very long):
  - shortened scripted shell pacing in `test.sh`.
  - reduced stress loop depth while preserving coverage.
  - lowered QEMU timeout windows.
  - added AHCI partition-node assertion (`sda1`) using generated MBR seed.
- Verification:
  - `make -j4` PASS
  - `./test.sh` PASS
  - AHCI boot log reports `mode=irq` when IRQ route is active.

Additional references used for this step:
- AHCI base programming model and reset/port control semantics:
  - https://docslib.org/doc/12302705/serial-ata-advanced-host-controller-interface-ahci
- GPT on-disk layout and partition entry fields:
  - https://uefi.org/specs/UEFI/2.10/05_GUID_Partition_Table_Format.html

### Step 39: AHCI hotplug hardening + ext4 ordered-write tightening + shorter SATA stress loop
- AHCI hotplug/link-change handling improvements (`kernel/ahci.c`):
  - added per-port slot provisioning helper (`ahci_configure_port_slot`) so all implemented AHCI ports in `PI` can be tracked, even when link is down at boot.
  - `ahci_init` now provisions slots from `PI` and runs initial presence probing per slot.
  - `ahci_poll` now:
    - provisions newly discovered `PI` ports (if any),
    - then re-checks link/presence state for all configured slots.
  - link transition path now uses explicit state flow:
    - link-down -> mark offline immediately,
    - link-up -> rebase + COMRESET + IDENTIFY, then mark online/offline.
  - IRQ link-event path now attempts late slot provisioning for ports that signal link events before a slot exists.
  - added explicit injected TFES marker:
    - `[ahci][fault] injected taskfile error`
  - retained and validated existing timeout/reset/partial hooks:
    - timeout, reset-storm, partial-transfer fault injection.

- Devfs hotplug usability path (`kernel/fs.c`):
  - added `fs_sync_hotplug_block_devs()` and `/dev` path-triggered sync in lookup path.
  - behavior:
    - when path access starts with `/dev`, kernel polls AHCI and auto-adds `/dev/sdX` nodes for newly present slots.
    - partition probing runs for newly created hotplugged block nodes.
  - this preserves existing “node appears only when present” behavior for newly inserted SATA media, without requiring reboot.

- ext4 crash-safety hardening (`kernel/ext4.c`):
  - tightened `ext4_write` ordered-write sequence for newly allocated file blocks:
    - allocate data block,
    - write data first,
    - checkpoint/flush,
    - then link metadata mapping (extent/direct pointer),
    - then checkpoint before inode size/block metadata store.
  - added failure cleanup for fresh-block write/link failures (`ext4_free_block` on unlinked fresh allocation paths).
  - final inode update is now followed by ordered checkpoint helper rather than a single generic flush call path.

- I/O stress/fault-injection suite updates (`stress_ahci.sh`):
  - reduced default runtime knobs:
    - `STEP=0.02`, `QEMU_TIMEOUT=90`, `LOOPS=24`.
  - reduced noisy log volume by default:
    - tail output only (`LOG_TAIL_LINES`, `VERBOSE=1` for full logs).
  - fault-injection profile uses denser intervals for faster hit rates:
    - timeout=9, error=11, reset=13, partial=17.
  - build path is now quieter (`run_make`) and faster:
    - no forced clean before normal run,
    - normal rebuild restore remains enabled by default (`RESTORE_BUILD=1`), with opt-out available (`RESTORE_BUILD=0`).
  - keeps panic/no-runnable gating and `[ahci][fault]` assertions.

- Test runtime reduction (`test.sh`):
  - reduced default pacing to `STEP=0.03`.
  - lowered qemu timeouts from `120 -> 95` and `35 -> 25`.

- Verification:
  - `make -j4` PASS
  - `./test.sh` PASS
  - `./stress_ahci.sh` PASS

- Remaining limitations / next safety targets:
  - `/dev/sdX` auto-removal on unplug is not implemented yet; unplugged device operations fail via backend presence checks, but node removal is deferred.
  - ext4 still has no full journal replay/transaction system; ordered checkpoints reduce risk, but do not equal full journaling guarantees.
  - a dedicated multi-sector userland AHCI exerciser would improve deterministic coverage of injected partial-transfer paths.

Additional references used for this step:
- AHCI 1.3.1 specification (port/link interrupt semantics, command engine/reset behavior):
  - https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/serial-ata-ahci-spec-rev1-3-1.pdf
- Linux ext4 journaling/ordered mode behavior:
  - https://www.kernel.org/doc/html/latest/filesystems/ext4/journal.html

### Step 40: Metadata journal+replay core, unified mmap/file cache coherence, AHCI detach cleanup
- Added ext4 metadata transaction log + replay core (`kernel/ext4.c`):
  - new transaction APIs:
    - `ext4_tx_begin()`
    - `ext4_tx_commit()`
    - `ext4_tx_abort()`
  - write interception in `ext4_disk_write_bytes` during active transaction:
    - writes are recorded into journal records/data region first.
    - home-location writes happen at commit apply phase.
  - read-your-writes support during active transaction:
    - `ext4_disk_read_bytes` overlays pending journaled writes on top of raw reads.
  - replay on mount:
    - committed journal records are re-applied before normal operation.
    - prep/incomplete/invalid headers are cleared.
  - journal placement:
    - reserved tail area in mounted block range (requires slack after ext4 blocks).
    - mount log now prints `jlog=on/off`.
  - unmount now aborts active tx if present and clears journal runtime state.

- Wired ext4 transaction boundaries from VFS (`kernel/fs.c`):
  - ext4 mutating paths now wrap operations in begin/commit/abort:
    - create file/dir
    - link/symlink
    - unlink/rmdir/rename
    - write
    - truncate
  - this centralizes ext4 write atomicity boundaries at syscall-visible filesystem operations.

- Unified page-cache + mmap/file I/O coherence (`kernel/pagecache.c`, `kernel/fs.c`, `include/pagecache.h`):
  - page cache no longer depends on `inode->data` only.
  - load/store paths now support both:
    - memfs files
    - ext4 files (`ext4_read`/`ext4_write` backing)
  - added cache-coherence helpers:
    - `pagecache_overlay_read(...)`
    - `pagecache_notify_write(...)`
    - `pagecache_invalidate_inode(...)`
  - `fs_read` overlays cached pages onto returned data for file reads.
  - `fs_write` updates cached pages for overlapping regions after successful write.
  - `fs_truncate` invalidates inode cache entries to prevent stale mappings.
  - `pagecache_writeback` now wraps ext4 writes in ext4 tx APIs.

- AHCI robustness completion increments (`kernel/ahci.c`, `kernel/fs.c`):
  - error classification added for command path:
    - timeout / taskfile-error / busy
  - timeout backoff added per disk (`fail_streak`, per-disk timeout cycles with cap).
  - retry recovery strategy now chooses port-level rebase/comreset before full fallback.
  - `/dev` unplug cleanup:
    - `fs_sync_hotplug_block_devs()` now removes `/dev/sdX` and `/dev/sdXn` nodes when drive is absent.
    - removal is guarded if that source device is currently mounted.

- ext4 image generation updated to preserve reserved tail region for journal area:
  - `run.sh`: mke2fs block count set for 64 MiB images (`16128` blocks).
  - `test.sh`: same reserve policy for seeded test image.
  - `stress_ahci.sh`: reserve on 128 MiB stress image (`32512` blocks).

- Verification:
  - `make -j4` PASS
  - `./test.sh` PASS
  - `./stress_ahci.sh` PASS

- Current limitations (explicit):
  - journal model is metadata transaction logging/replay in FuriOS format, not full on-disk JBD2 compatibility.
  - jlog requires slack tail space in mounted block range; if unavailable, mount runs with `jlog=off`.
  - detach node removal does not yet perform FD-level orphan tracking for already-open `/dev/sdX` handles.

Additional references used for this step:
- ext4 journaling behavior documentation:
  - https://www.kernel.org/doc/html/latest/filesystems/ext4/journal.html
- ext4 on-disk layout/field references:
  - https://www.kernel.org/doc/html/latest/filesystems/ext4/index.html
- AHCI base spec reference:
  - https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/serial-ata-ahci-spec-rev1-3-1.pdf

### Step 41: Begin full JBD2 on-disk compatibility (mount-time replay path)
- Replaced the old mount-time “custom tail log” path for journaled ext4 volumes with JBD2 parsing/replay (`kernel/ext4.c`):
  - new on-disk JBD2 constants and parsers:
    - big-endian block header decode (`magic`, `blocktype`, `sequence`)
    - descriptor block tag decode (legacy + `JBD2_FEATURE_INCOMPAT_CSUM_V3`)
    - revoke block decode (`r_count`, 32/64-bit block entries)
    - commit block boundary handling
  - internal journal inode replay:
    - reads `s_journal_inum` from ext4 superblock.
    - maps journal logical blocks through inode extents/direct maps.
    - replays committed, non-revoked metadata/data blocks to home locations.
    - handles `JBD2_FLAG_ESCAPE` restoration (`h_magic` word restore on replayed block head).
  - recovery finalization:
    - after successful replay, updates journal superblock (`s_sequence`, `s_start=0`).
    - clears ext4 `INCOMPAT_RECOVER` bit once replay has been durably checkpointed.

- Mount flow split for journal modes:
  - ext4 with `has_journal`:
    - uses new JBD2 replay path.
    - reports `jbd2=on` in mount log.
  - ext4 without `has_journal`:
    - keeps existing FuriOS custom metadata tx log path (`jlog=on/off`) unchanged.

- Safety/compatibility guards added:
  - rejects external journal devices (`s_journal_inum=0 && s_journal_dev!=0`) for now.
  - validates journal geometry (`blocksize`, `first`, `maxlen`, `start`) before replay.
  - bounded replay scan guard prevents endless ring traversal on malformed logs.

- Verification (single short validation pass):
  - `make -j4` PASS
  - Short QEMU smoke with journaled image (`has_journal,64bit,metadata_csum`) PASS:
    - ext4 mounted as `ro` with `jbd2=on`
    - shell remained interactive
    - expected write rejection on read-only mount verified

- Current JBD2 scope after this step:
  - implemented:
    - on-disk journal header/tag/revoke/commit parsing
    - committed transaction replay for internal journal inode
  - not yet implemented:
    - full JBD2 transaction writer for ext4 RW-on-journaled volumes
    - checksum verification enforcement for descriptor/revoke/commit blocks
    - external journal device support
    - fast-commit replay path

Additional references used for this step:
- ext4/jbd2 on-disk format (block headers, descriptor/revoke/commit layout):
  - https://www.kernel.org/doc/html/latest/filesystems/ext4/journal.html
- journaling recovery model (three-pass description in Linux API docs):
  - https://www.kernel.org/doc/html/v5.8/filesystems/journalling.html
- ext4 superblock field layout (journal inode/device fields):
  - https://android.googlesource.com/platform/external/e2fsprogs/+/refs/heads/main/lib/ext2fs/ext2_fs.h

### Step 42: Strict JBD2 replay validation + real on-disk JBD2 transaction writer
- JBD2 strict replay validation hardening (`kernel/ext4.c`):
  - added CRC32C implementation for journal checksum verification paths.
  - added strict sequence-ordered replay state machine:
    - enforces expected `h_sequence` continuity per in-flight transaction.
    - only applies updates after matching commit block.
  - added checksum verification before applying blocks:
    - descriptor block tail checksum (when journal checksum features are active),
    - revoke block tail checksum (when active),
    - commit block checksum/type/size fields (when active),
    - per-tag data block checksum validation (`uuid + seq + data`) for v2/v3.
  - fixed ext4 superblock journal field offsets:
    - `s_journal_inum` / `s_journal_dev` now read from correct offsets.

- New in-memory JBD2 transaction engine (`kernel/ext4.c`):
  - added tx handle flow:
    - `ext4_jbd2_tx_start()`
    - `ext4_jbd2_tx_write_bytes()` (journal_write path)
    - `ext4_jbd2_tx_commit()` / `ext4_jbd2_tx_abort()`
  - integrates with existing syscall-visible tx API:
    - `ext4_tx_begin/commit/abort` now dispatch to JBD2 tx engine when mounted ext4 uses journal.
  - tracks dirty blocks in-memory during transaction and provides read-your-writes overlay for tx reads.

- Real JBD2 on-disk transaction emission:
  - transaction commit now writes:
    - descriptor block(s) with feature-dependent tag layout:
      - v3 tag format (16-byte tags with 32-bit checksum),
      - legacy/v2 tag format (8/12-byte tags with 16-bit checksum for v2),
    - journaled data blocks (with escape handling for magic-at-offset-0),
    - commit block (checksum-bearing when journal checksum features are active).
  - journal superblock (`s_start`, `s_sequence`) is updated on transaction begin/finish and checksummed when needed.
  - after journal commit flush, home blocks are checkpointed, then journal is marked clean (`s_start=0`).

- Compatibility policy:
  - replay supports checksum and non-checksum journal modes.
  - write path supports v2/v3/non-checksum descriptor/tag formats.
  - ext4 remains read-only when existing ext4 RO-compat constraints are active (e.g. metadata checksum flags), but journaled rw mount now works for supported non-RO-compat images.

- Verification (single short focused pass set):
  - `make -j4` PASS
  - Journaled ext4 (non-RO-compat, has_journal) QEMU pass:
    - mounted `rw` with `jbd2=on`
    - write/read smoke path succeeded
  - Journaled ext4 ro smoke still mounts and reads correctly where RO policy applies.

- Current limitations:
  - no external journal device support yet.
  - no ring-space/tail checkpoint allocator policy yet (current writer uses conservative bounded placement from journal first block per tx).
  - checksum strictness follows feature availability; full coverage requires checksum-enabled journal format.

Additional references used for this step:
- Linux ext4/journal format and checksum fields:
  - https://www.kernel.org/doc/html/latest/filesystems/ext4/journal.html
- Linux `jbd2` header layout (commit/tag/tail structures):
  - https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/include/linux/jbd2.h
- e2fsprogs ext4 superblock layout (`s_journal_inum`/`s_journal_dev` offsets):
  - https://android.googlesource.com/platform/external/e2fsprogs/+/refs/heads/main/lib/ext2fs/ext2_fs.h

### Step 43: JBD2 ring/head correctness hardening and compatibility gaps closed
- JBD2 superblock/ring handling improvements (`kernel/ext4.c`):
  - added explicit `s_head` handling in runtime state and journal superblock writeback.
  - writer now reserves transaction space from journal ring head (not hardcoded `s_first`), with wrap-safe reservation logic.
  - transaction begin/commit updates now maintain:
    - `s_start` (active transaction start),
    - `s_sequence` (next transaction id),
    - `s_head` (next free log position).
  - replay cleanup now updates `s_head` to scan end and clears stale `s_start` even when no committed tx is replayed.

- Descriptor/parser compatibility hardening:
  - fixed v3 descriptor tag parsing to handle optional UUID field when `JBD2_FLAG_SAME_UUID` is not set.
  - tightened journal feature gating:
    - rejects unsupported JBD2 incompat feature bits.
    - rejects active fast-commit journal mode (`JBD2_FEATURE_INCOMPAT_FAST_COMMIT` + nonzero fc block count), which is not implemented yet.

- Writer compatibility updates:
  - descriptor emission now supports feature-dependent tag sizes:
    - v3 (16-byte tags),
    - legacy/v2 (8/12-byte tags).
  - data block placement for descriptor payloads is now wrap-safe via ring `next_block` stepping.

- Verification (single short pass):
  - `make -j4` PASS
  - two-boot persistence smoke with journaled ext4 image:
    - boot #1: mounted `rw` with `jbd2=on`, wrote `/mnt/persist.txt`
    - boot #2: mounted `rw` with `jbd2=on`, readback matched (`jbd2-persist`)

- Remaining “full JBD2” items still open:
  - fast-commit on-disk compatibility.
  - richer revoke emission policy from mutators (current path supports parsing/replay and optional revoke blocks, but does not yet generate revoke records for all metadata free/overwrite paths).
  - multi-transaction log occupancy/tail advancement policy beyond immediate checkpoint-per-tx behavior.

Additional references used for this step:
- ext4 journal internals (j_head/j_tail/s_start/s_sequence behavior):
  - https://www.kernel.org/doc/html/latest/filesystems/ext4/journal.html
- Linux JBD2 structure definitions (`journal_superblock_t`, tag formats, feature bits):
  - https://codebrowser.dev/linux/linux/include/linux/jbd2.h.html

### Step 44: Revoke emission + tail-aware ENOSPC + deferred checkpoint batching
- Implemented JBD2 revoke emission on metadata free paths (`kernel/ext4.c`):
  - `ext4_free_block()` now records revoke candidates during active JBD2 tx.
  - tx writer emits on-disk JBD2 revoke blocks (`JBD2_REVOKE_BLOCK`) with feature-aware 32/64-bit block entries.
  - revoke list is de-duplicated per tx and reconciled against same-tx re-dirty operations.

- Added proper ring accounting with head/tail semantics and ENOSPC behavior:
  - introduced runtime tail/start tracking derived from oldest uncheckpointed tx.
  - `ext4_jbd2_reserve_ring()` now computes free space against live head/tail state and refuses reservation when insufficient space exists.
  - this blocks transaction commits safely instead of overwriting uncheckpointed journal regions.

- Added deferred transaction batching/checkpoint model (no immediate home-block checkpoint per syscall):
  - committed txs are now queued as pending journal transactions with in-memory dirty block snapshots.
  - ext4 reads overlay uncheckpointed pending tx dirty blocks, so userspace observes committed filesystem state before checkpoint.
  - checkpoint worker path:
    - `ext4_jbd2_maybe_checkpoint()` triggers by pending-depth and timer-age policy.
    - `ext4_jbd2_checkpoint_some()` checkpoints a bounded number of pending txs and advances tail/start.
  - unmount forces checkpoint drain of pending txs before teardown.

- JBD2 superblock state updates were aligned with batching:
  - `s_start` reflects oldest uncheckpointed tx (or 0 when clean).
  - `s_sequence` written as oldest pending seq when journal dirty, otherwise next seq.
  - `s_head` advanced as journal writer reserves/commits tx ranges.

- Stability fix during this step:
  - corrected JBD2 apply logic to allow valid filesystem block 0 updates (needed for ext4 superblock-region journaled updates).
  - removed large-stack copy in checkpoint path (pointer-based pending access) to avoid kernel trap regressions.

- Verification (single short focused run):
  - `make -j4` PASS
  - 2-boot QEMU journaled ext4 scenario PASS:
    - boot #1: create/write/unlink/mkdir/mv/read on `/mnt`
    - boot #2: post-reboot readback consistency confirmed
  - shell remained interactive in `-nographic` path.

- Remaining high-impact JBD2 items:
  - full multi-pass revoke-aware recovery table across many uncheckpointed tx generations (current replay is strict and validated, but can still be tightened further to Linux-style pass ordering).
  - explicit background kernel checkpoint thread/scheduler entity (current model is deferred+bounded checkpoint policy invoked in ext4 tx flow).

Additional references used for this step:
- ext4 journaling and recovery behavior:
  - https://www.kernel.org/doc/html/latest/filesystems/ext4/journal.html
- Linux jbd2 structure/feature definitions:
  - https://codebrowser.dev/linux/linux/include/linux/jbd2.h.html

### Step 45: Practical final hardening pass (JBD2 safety + faster validation loop)
- JBD2 safety fixes in `kernel/ext4.c`:
  - fixed pre-commit superblock staging so `s_start` is only set to the current tx start when there are no older pending txs.
    - when pending txs exist, `s_start` now keeps pointing at the oldest uncheckpointed tx.
  - made ring reservation logic strictly distance-based with wrap-safe helpers:
    - `ext4_jbd2_ring_advance()`
    - `ext4_jbd2_ring_distance()`
    - prevents head/tail collision edge cases across wrap.
  - tightened replay validation:
    - reject descriptor tags with unknown flag bits.
    - reject revoke blocks when journal revoke feature is not advertised.
  - added an early queue bound check in commit path:
    - refuse commit if `pending_count` is already at `EXT4_JBD2_MAX_PENDING_TX`.
  - forced flush after pre-commit super update to make reservation/start state durable before descriptor/data/commit emission.

- Test runtime trimming in `test.sh`:
  - default command pacing reduced: `STEP=0.02` (from `0.03`).
  - long write loop shortened: 6 lines (from 10).
  - QEMU timeout windows tightened:
    - main run `70s` (from `95s`)
    - AHCI run `18s` (from `25s`)

- Verification (single-pass, short):
  - `make -j4` PASS
  - short JBD2 RW smoke PASS:
    - mount log includes `jbd2=on`
    - create/write/remove directory/file flow on `/mnt` succeeded
  - short crash-replay check PASS:
    - boot #1 writes `/mnt/a` and `/mnt/b`, then forced stop
    - boot #2 reported `[ext4] jbd2 replay tx=1` and both files read back (`one`, `two`)

Additional references used for this step:
- ext4 journal start/sequence recovery semantics:
  - https://www.kernel.org/doc/html/latest/filesystems/ext4/journal.html
- Linux jbd2 on-disk flags/feature definitions:
  - https://codebrowser.dev/linux/linux/include/linux/jbd2.h.html

### Step 46: Native `/sbin/mkfs.ext4` (EL0 command + kernel formatter syscall)
- Added a real ext4 formatter path in-kernel:
  - new syscall `SYS_MKFSEXT4` added to central syscall list (`include/syscall.h`) and dispatcher (`kernel/syscall.c`).
  - new VFS API `fs_mkfs_ext4(target, flags)` in `kernel/fs.c` / `include/fs.h`.
  - formatter currently writes a clean ext4 (no journal feature enabled) compatible with FuriOS ext4 mount/write path:
    - 4KiB block size, little-endian on-disk structures.
    - block group descriptors (`bg_block_bitmap_lo`, `bg_inode_bitmap_lo`, `bg_inode_table_lo`).
    - block/inode bitmaps, inode tables.
    - root inode (`#2`) and `lost+found` inode (`#11`) with valid directory entries (`.`, `..`, `lost+found`).
    - feature flags emitted for compatibility with current kernel support (`filetype` incompat bit, no metadata checksum/journal requirements).
  - safety guards:
    - refuses formatting if target is not a ready block device node.
    - refuses formatting if the underlying source device is currently mounted.
    - uses block-cache attachment + flush to keep cache/device coherence.

- Added EL0 userspace command in `/sbin`:
  - new program: `user/mkfs.ext4.c`.
  - embedded in initfs under `/sbin/mkfs.ext4` (not `/bin`) via `kernel/fs.c`.
  - shell command resolution updated to fall back from `/bin/<cmd>` to `/sbin/<cmd>` for slash-less commands (`user/sh.c`).

- Build wiring:
  - added `mkfs.ext4` to `USER_PROGS` in `Makefile`.

- Test updates (kept short):
  - `test.sh` now runs a compact mkfs flow:
    - `umount /mnt`
    - `mkfs.ext4 /dev/vda`
    - remount + create/read smoke under new filesystem.
  - existing shortened runtime policy remains in place.

- Verification:
  - `make -j4` PASS
  - `./test.sh` PASS
  - observed runtime:
    - `[mkfs.ext4] formatted /dev/vda ...`
    - `mkfs.ext4: formatted`
    - remount succeeded and post-format file ops passed.

Additional references used for this step:
- ext4 superblock layout:
  - https://www.kernel.org/doc/html/next/filesystems/ext4/super.html
- ext4 block group descriptor layout:
  - https://www.kernel.org/doc/html/latest/filesystems/ext4/group_descr.html
- ext4 directory entry format (`ext4_dir_entry_2`):
  - https://www.kernel.org/doc/html/latest/filesystems/ext4/directory.html
- ext4/inode `i_blocks_lo` semantics:
  - https://www.kernel.org/doc/html/latest/filesystems/ext4/inodes.html

### Step 47: mkfs.ext4 advanced feature/options expansion
- Extended mkfs syscall ABI and userspace option plumbing:
  - `SYS_MKFSEXT4` now accepts an options struct (`fu_mkfs_ext4_opts_t`) passed from EL0.
  - Added option fields/flags in `include/syscall.h`:
    - feature mask: `extents`, `64bit`, `metadata_csum`, `sparse_super`.
    - knobs: label, UUID, reserved blocks percent (`-m`), stride (`-E stride=N`).
  - User syscall wrapper and command updated:
    - `sys_mkfsext4(path, flags, opts)` in `user/lib/syscall.c`.
    - `/sbin/mkfs.ext4` parser now supports:
      - `-L <label>`
      - `-U <uuid|random>`
      - `-m <0..99>`
      - `-E stride=<n>`
      - `-O extents,64bit,metadata_csum,sparse_super` (+ `^feature`, `none`)

- Formatter internals in `kernel/fs.c` upgraded:
  - Extents formatting:
    - sets `INCOMPAT_EXTENTS` when requested.
    - writes extent-rooted inode trees (`EXT4_EXTENTS_FL`) for `/` and `lost+found`.
  - 64-bit formatting:
    - sets `INCOMPAT_64BIT` and `s_desc_size=64`.
    - writes high superblock counters (`blocks/free/r_blocks hi`).
  - Sparse super support:
    - group layout now respects sparse-super super/GDT backup placement policy.
    - writes backup super/GDT copies in eligible backup groups.
  - Metadata checksum mode:
    - sets `RO_COMPAT_METADATA_CSUM`.
    - writes superblock checksum type/checksum.
    - writes per-group descriptor checksums and bitmap checksums.
  - Option-driven fields:
    - writes volume label, UUID override, reserved block count, and RAID stride.

- Process arg-capacity hardening:
  - Increased `MAX_ARGV` from `8` to `16` (`include/config.h`) so complex tool commands are not truncated.

- Runtime behavior after this step:
  - Default mkfs output remains mountable/writable on current kernel policy.
  - `metadata_csum` images mount read-only by design in current mount policy (`write_enabled=false` under metadata checksum rocompat).

- Verification:
  - `make -j4` PASS
  - `./test.sh` PASS with advanced mkfs invocation:
    - `mkfs.ext4 -L FuriOSVOL -O extents,64bit,sparse_super -m 1 -E stride=8 /dev/vda`
    - remount + create/read smoke passed.
  - targeted metadata checksum check PASS:
    - `mkfs.ext4 -O extents,64bit,metadata_csum,sparse_super /dev/vda`
    - remount logged `mounted ro` and write attempt correctly failed.

Additional references used for this step:
- ext4 superblock fields (`s_desc_size`, 64-bit counters, raid stride, checksum fields):
  - https://www.kernel.org/doc/html/latest/filesystems/ext4/super.html
- ext4 group descriptor fields/checksum-related members:
  - https://www.kernel.org/doc/html/latest/filesystems/ext4/group_descr.html
- ext4 extent tree on-disk format:
  - https://www.kernel.org/doc/html/latest/filesystems/ext4/ifork.html

### Step 48: `run.sh` defaults to SATA SSD image provisioning
- Updated runtime launch flow so a SATA image is ready by default for manual formatting tests:
  - `run.sh` now auto-creates `build/ssd.img` (default size `256M`) unless disabled.
  - AHCI/SATA attach is enabled by default and mapped via `ich9-ahci` + `ide-hd` as `sd0` (`/dev/sda` in FuriOS).
  - Existing virtio disk behavior remains intact, so current workflows are not broken.
- New env controls in `run.sh`:
  - `SSD_DISK_PATH` (default `build/ssd.img`)
  - `SSD_DISK_SIZE` (default `256M`)
  - `DISABLE_AHCI=1` to boot without SATA
  - existing `DISABLE_VIRTIO=1` can be combined to boot with only SATA (`/dev/sda`) for clean mkfs targeting.
- Verification:
  - short boot probe confirms `/dev/sda` appears with updated default launch path.

### Step 49: SATA mkfs hang fix + SATA-only default run profile
- Addressed observed `mkfs.ext4 /dev/sda` stall under AHCI path:
  - Root cause was full-device zeroing in formatter (`for b in total_blocks: zero block`) before metadata layout writes.
  - Removed whole-disk zero pass from `fs_mkfs_ext4`; formatter now writes only required ext4 metadata/data bootstrap blocks (super/GDT/bitmaps/inode tables/root/lost+found + sparse backups).
  - Result: AHCI formatting latency dropped from “appears hung” to immediate completion in interactive shell.
- Updated `run.sh` to default to SATA-only boot target:
  - Virtio disk is now opt-in (`ENABLE_VIRTIO=1`) instead of default-on.
  - Default boot now exposes `/dev/sda` only, matching manual mkfs workflow.
  - Existing `DISABLE_VIRTIO` remains respected if explicitly set.
- Verification (single short probe):
  - `make -j4` PASS.
  - Non-graphical boot via `./run.sh` showed `/dev/sda` only.
  - Interactive sequence passed:
    - `mkfs.ext4 /dev/sda`
    - `mount /dev/sda /mnt ext4`
    - `ls /mnt` -> `lost+found`
  - Confirmed safety gate still works:
    - `mkfs.ext4 /dev/sda` returns `failed` while `/dev/sda` is mounted; succeeds after `umount /mnt`.

### Step 50: ext4 mkfs journaling + profile presets + offline fsck
- Added real internal journal creation in formatter:
  - mkfs now supports `has_journal` feature in `-O`.
  - default feature set now includes `has_journal` (unless explicitly disabled via `^has_journal`).
  - formatter writes:
    - superblock journal fields (`s_feature_compat HAS_JOURNAL`, `s_journal_inum`, `s_jnl_blocks` copy),
    - journal inode (`inode #8`) with valid block mapping,
    - JBD2 superblock in journal data area (`magic/type/sequence/geometry`).
  - mount path now reports `jbd2=on` for mkfs-created journaled filesystems.

- Added mkfs profile/ratio options (Linux-style workflow knobs):
  - userspace parser now supports:
    - `-T default|small|largefile`
    - `-i <bytes-per-inode>`
    - `-E journal_blocks=<n>` (journal geometry control)
  - kernel formatter computes dynamic `inodes_per_group` and inode table size from profile/ratio.
  - ioctl/syscall ABI extended in `fu_mkfs_ext4_opts_t` with profile + bytes-per-inode + journal size fields.

- Added offline checker command `/sbin/fsck.ext4`:
  - new syscall `SYS_FSCKEXT4` and kernel entrypoint `fs_fsck_ext4(...)`.
  - checker validates:
    - superblock geometry and feature sanity,
    - GDT entry ranges/counts,
    - block/inode bitmap free-count consistency vs descriptors,
    - inode bitmap vs inode-table mode consistency,
    - root inode presence/type,
    - journal inode + JBD2 superblock signature when journal feature is enabled,
    - aggregate free counts vs superblock counters.
  - userspace tool:
    - `/sbin/fsck.ext4 <device>` (returns clean/fail).

- Build + initfs wiring:
  - added `fsck.ext4` to `USER_PROGS` in `Makefile`.
  - embedded in initfs under `/sbin/fsck.ext4` via `kernel/fs.c`.

- Test updates:
  - `test.sh` now includes `fsck.ext4 /dev/vda` after mkfs, before remount.
  - asserts `fsck.ext4: clean`.

- Verification:
  - `make -j4` PASS
  - `./test.sh` PASS
  - targeted SATA checks PASS:
    - journaled format:
      - `mkfs.ext4 -T largefile -i 65536 -O extents,64bit,sparse_super,has_journal /dev/sda`
      - `fsck.ext4 /dev/sda` -> clean
      - `mount /dev/sda /mnt ext4` -> mounted with `jbd2=on`
    - non-journal format:
      - `mkfs.ext4 -O extents,sparse_super,^has_journal /dev/sda`
      - `fsck.ext4 /dev/sda` -> clean
      - mount shows `jlog=off`.

Additional references used for this step:
- ext4 superblock fields (`s_journal_inum`, `s_jnl_blocks`, feature bits):
  - https://www.kernel.org/doc/html/latest/filesystems/ext4/super.html
- ext4 journal internals / JBD2 block format:
  - https://www.kernel.org/doc/html/latest/filesystems/ext4/journal.html
- mke2fs options (`-T`, `-i`) and behavior:
  - https://man7.org/linux/man-pages/man8/mke2fs.8.html
- e2fsprogs profile defaults (`small`, `largefile`) from `mke2fs.conf.in`:
  - https://github.com/tytso/e2fsprogs/blob/master/misc/mke2fs.conf.in

### Step 51: JBD2 feature-bit alignment + stricter journal checks in fsck
- Fixed JBD2 incompat bit definitions in runtime ext4 journal code to match Linux JBD2 values:
  - `REVOKE=0x1`, `64BIT=0x2`, `ASYNC=0x4`, `CSUM_V2=0x8`, `CSUM_V3=0x10`, `FAST_COMMIT=0x20`.
  - This removes formatter/runtime mismatch risk for future checksum/64-bit journal feature use.

- Hardened offline checker (`fsck.ext4`) journal validation:
  - If ext4 `has_journal` is set, checker now requires nonzero `s_journal_inum`.
  - For journal inode superblock, checker now validates:
    - blocksize/geometry (`block_size`, `maxlen`, `first`),
    - required revoke capability bit in JBD2 incompat flags,
    - no unknown incompat bits outside currently supported set.
  - Existing signature checks (`magic`, `type`) remain in place.

- Verification:
  - `make -j4` PASS
  - `./test.sh` PASS
    - includes mkfs + fsck + remount flow and AHCI device probe flow.

Additional references used for this step:
- Linux JBD2 on-disk feature bit definitions:
  - https://codebrowser.dev/linux/linux/include/linux/jbd2.h.html
- ext4 journaling/JBD2 behavior:
  - https://www.kernel.org/doc/html/latest/filesystems/ext4/journal.html

### Step 52: fsck repair modes + deeper offline invariants + stricter ext4 feature gating
- Added real `fsck.ext4` mode flags in userspace:
  - `/sbin/fsck.ext4 -n <dev>`: no-repair check-only.
  - `/sbin/fsck.ext4 -p <dev>`: preen auto-repair path.
  - `/sbin/fsck.ext4 -y <dev>`: force auto-repair path.
  - conflicting mode combinations are rejected.
  - syscall return handling now distinguishes clean vs repaired.

- Extended kernel offline checker (`fs_fsck_ext4`) invariants:
  - compatibility-class reporting at start:
    - `rw-ok`, `ro-only`, or `unsupported`.
  - duplicate data-block detection across inode data mappings.
  - inode block-reference vs block-bitmap consistency checks.
  - inode link-count recomputation from full directory entry traversal.
  - directory graph checks:
    - validates `.` and `..` presence/targets,
    - parent connectivity to root (`/`) and loop detection.
  - orphan handling:
    - detects nonzero `s_last_orphan`,
    - clears orphan head in repair mode.
  - extent-tree structural validation:
    - header magic/depth/capacity checks,
    - monotonic logical block ordering,
    - physical block-range validation.
  - metadata checksum verification (when `metadata_csum` set):
    - superblock checksum type/checksum,
    - group descriptor checksum,
    - block/inode bitmap checksums.

- Repair actions implemented (`-p`/`-y`):
  - fixes per-group free block/inode counters in GDT.
  - fixes per-group used-directory counters in GDT.
  - fixes superblock free block/inode counters.
  - fixes inode `i_links_count` values to recomputed references.
  - clears stale `s_last_orphan`.
  - recomputes/writes metadata checksums as needed.
  - writes/flushes repaired metadata atomically through block cache.

- Stricter mount-time feature gating (`ext4_mount`):
  - unknown `incompat` bits: hard fail with exact mask.
  - unknown `ro_compat` bits: mount forced read-only with exact mask log.
  - expanded known safe `ro_compat` set to include common Linux bits:
    - `LARGE_FILE`, `BTREE_DIR`, `HUGE_FILE`, `DIR_NLINK`, `EXTRA_ISIZE`,
      `SPARSE_SUPER`, `GDT_CSUM`, `METADATA_CSUM`.
  - this avoids false read-only downgrades on normal Linux-created ext4 while preserving strict behavior.

- Verification:
  - `make -j4` PASS.
  - `./test.sh` PASS (single full pass), including:
    - boot/mount/write flow,
    - mkfs + fsck clean path,
    - remount and ext4 mutation flow,
    - AHCI probe/dev-node flow.

Additional references used for this step:
- ext4 feature compatibility classes (compat/incompat/ro_compat):
  - https://www.kernel.org/doc/html/latest/filesystems/ext4/super.html
- ext4 directory entry format and semantics:
  - https://www.kernel.org/doc/html/latest/filesystems/ext4/directory.html
- ext4 inode/extents on-disk structure:
  - https://www.kernel.org/doc/html/latest/filesystems/ext4/inodes.html
  - https://www.kernel.org/doc/html/latest/filesystems/ext4/ifork.html
- ext4 journal internals/JBD2:
  - https://www.kernel.org/doc/html/latest/filesystems/ext4/journal.html

### Step 53: NVMe PCI driver + /dev/nvmeXn1 devfs integration + QEMU multi-NVMe wiring
- Added new kernel NVMe driver:
  - files:
    - `include/nvme.h`
    - `kernel/nvme.c`
  - PCI ECAM probe for Mass Storage/NVM class devices.
  - BAR assignment + MMIO map (with non-overlapping allocator window to avoid AHCI BAR collisions).
  - Controller bring-up sequence:
    - disable controller,
    - program AQA/ASQ/ACQ,
    - set CC (including MPS/IOSQES/IOCQES),
    - wait RDY.
  - Admin + I/O queue setup (single I/O queue pair).
  - Identify Controller + Identify Namespace path.
  - Namespace exposure currently targets NSID 1 per controller and 512-byte LBA mode.
  - NVM read/write/flush commands implemented (polling completions).
  - IRQ hook wired (`nvme_handle_irq`) for trap dispatch path compatibility.

- Device model integration:
  - extended `dev_kind_t` with:
    - `DEV_NVME0N1` .. `DEV_NVME7N1`.
  - `kernel/fs.c`:
    - block backend abstraction now routes NVMe I/O/capacity/readiness/flush.
    - `/dev` population includes `nvme0n1..nvme7n1` when present.
    - hotplug sync path updated to create/remove NVMe nodes only when active.
    - partition naming updated:
      - SATA/virtio: `sda1`, `vda1`
      - NVMe: `nvme0n1p1` (adds `p` separator when base name ends in a digit).
    - boot auto-mount fallback now checks `/dev/nvme0n1p1` and `/dev/nvme0n1`.
  - `kernel/block_cache.c`:
    - NVMe devices supported as cache backends.
  - `kernel/kernel.c`:
    - `nvme_init()` in boot init order.
  - `kernel/trap.c`:
    - NVMe IRQ branch added.

- Build/run integration:
  - `Makefile` now compiles `kernel/nvme.o`.
  - `run.sh`:
    - fixed `DISABLE_AHCI=1` behavior so AHCI device is not created.
    - added NVMe image/device provisioning:
      - `NVME_COUNT` (default `0`, capped to `8`)
      - `ENABLE_NVME=1` helper (sets one NVMe disk when `NVME_COUNT` is left at `0`)
      - `NVME_DISK_SIZE` (default `256M`)
      - `NVME_DISK_PREFIX` (default `build/nvme`)
      - optional disable via `DISABLE_NVME=1`
    - multi-controller QEMU launch wiring:
      - creates `nvme0`, `nvme1`, ... and maps to `/dev/nvme0n1`, `/dev/nvme1n1`, ...

- Verification (single-pass style):
  - `make -j4` PASS.
  - NVMe-only boot smoke:
    - `DISABLE_VIRTIO=1 DISABLE_AHCI=1 NVME_COUNT=2 ./run.sh`
    - observed:
      - `[nvme] ready ctrl=0 ...`
      - `[nvme] ready ctrl=1 ...`
      - `/dev` contains `nvme0n1` and `nvme1n1`.
  - NVMe mkfs+mount smoke:
    - `mkfs.ext4 /dev/nvme0n1` PASS
    - `mount /dev/nvme0n1 /mnt ext4` PASS
    - `ls /mnt` shows `lost+found`.

Additional references used for this step:
- QEMU NVMe device model usage/parameters:
  - https://www.qemu.org/docs/master/system/devices/nvme.html
- QEMU ARM `virt` machine context:
  - https://www.qemu.org/docs/master/system/arm/virt.html
- Linux NVMe command/register definitions (queue create, identify, rw, CAP register layout):
  - https://codebrowser.dev/linux/linux/include/linux/nvme.h.html

### Step 54: NVMe hardening + multi-namespace support + non-512 LBA + mmap/page-cache coherence
- Hardened NVMe submit/completion and recovery path (`kernel/nvme.c`):
  - converted to controller+namespace model:
    - up to `NVME_MAX_CONTROLLERS` controllers,
    - up to `NVME_MAX_NAMESPACES` namespaces per controller.
  - interrupt-oriented completion path:
    - CQ interrupt enable + unmask (`INTMC`),
    - per-controller IRQ event counters,
    - submit loop now advances from IRQ events with polling fallback.
  - timeout/error hardening:
    - explicit submit result classes (`timeout`, `CFS`, status, CID mismatch),
    - retry gating on decoded status (`DNR`/SCT-based),
    - controller recovery/reinit path (disable->rebuild queues->re-identify namespaces).
  - non-512 LBA namespace support:
    - keeps 512-byte kernel block interface,
    - translates to namespace LBA size with per-block read-modify-write for partial writes.

- Expanded NVMe namespace exposure and dev naming:
  - `dev_kind_t` changed from fixed `DEV_NVME0N1..DEV_NVME7N1` to a range:
    - `DEV_NVME_BASE .. DEV_NVME_LAST`.
  - kind decode/encode now maps to `(controller, nsid)` pairs.
  - `/dev` now supports multi-namespace naming:
    - `nvme0n1`, `nvme0n2`, ... with partition names `nvme0n1p1`, etc.
  - touched files:
    - `include/fs.h`
    - `kernel/fs.c`
    - `kernel/block_cache.c`
    - `include/nvme.h`

- Shared IRQ robustness:
  - `kernel/trap.c` storage IRQ dispatch changed from single `else-if` chain to fan-out handling.
  - allows AHCI + NVMe shared INTx delivery on same IRQ line.

- QEMU NVMe topology/runtime expansion (`run.sh`):
  - `NVME_COUNT` controllers (default `0`, max `8`).
  - `NVME_NS_PER_CTRL` namespaces per controller (default `1`, max `8`).
  - `NVME_LBA_SIZE` namespace logical/physical block size (default `512`) for non-512 validation.
  - per-namespace image naming:
    - `${NVME_DISK_PREFIX}<ctrl>n<ns>.img`.

- Added short crash/fault recovery suite for ext4 + NVMe:
  - new script: `stress_nvme.sh`
  - phase 1:
    - format, mount, journaled writes, forced timeout kill (simulated crash/power-cut).
  - phase 2:
    - reboot on same image, unmount auto-mount, `fsck.ext4 -p`, remount, data/read-write checks, clean unmount.
  - script result: `[stress-nvme] PASS`.

- Improved mmap/page-cache coherence (`kernel/task.c`):
  - file-backed `MAP_PRIVATE` fault path now sources data from page cache (`pagecache_get_or_create`) instead of direct `inode->data`.
  - removed stale `inode->data` dependency from shared-map writeback guard.
  - effect:
    - works for ext4-backed file mappings,
    - better coherence between file I/O and mmap-backed pages.

- Verification (single-pass, short targeted):
  - `make -j4` PASS.
  - NVMe-only boot PASS (`/dev/nvme0n1`).
  - multi-namespace PASS:
    - `NVME_COUNT=1 NVME_NS_PER_CTRL=2` -> `/dev/nvme0n1`, `/dev/nvme0n2`.
  - non-512 LBA PASS:
    - `NVME_LBA_SIZE=4096`,
    - `mkfs.ext4 /dev/nvme0n1`, mount, list directory -> PASS.
  - mixed AHCI+NVMe shared-IRQ boot PASS.
  - crash/recovery script PASS (`./stress_nvme.sh`).

Additional references used for this step:
- QEMU NVMe controller/namespace topology:
  - https://www.qemu.org/docs/master/system/devices/nvme.html
- Linux NVMe structures/status model:
  - https://codebrowser.dev/linux/linux/include/linux/nvme.h.html
- NVMe base specification source:
  - https://nvmexpress.org/developers/nvme-specification/

### Step 55: Unified file cache write path + timer writeback + JBD2 ring/maintenance hardening + mkfs/fsck policy upgrades
- Unified regular-file I/O through page cache (`kernel/pagecache.c`, `kernel/fs.c`):
  - added cache-native APIs:
    - `pagecache_read`, `pagecache_write`
    - `pagecache_mark_dirty`
    - `pagecache_flush_inode`, `pagecache_flush_all`
    - `pagecache_tick` (bounded periodic writeback).
  - `fs_read/fs_write` for regular files now route through cache for both memfs/ext4.
  - cache entries now track:
    - `dirty` state,
    - LRU-ish `last_touch`,
    - eviction with writeback of dirty victims.
  - delayed writeback now persists through cache backend:
    - memfs -> inode data copy,
    - ext4 -> `ext4_tx_begin` + `ext4_write` + `ext4_tx_commit`.

- mmap coherence/writeback integration:
  - shared writable file mappings now mark cache pages dirty on map fault.
  - shared-page unmap path (`pagecache_writeback`) now marks cache-dirty rather than forcing immediate synchronous write.
  - keeps file read/write/mmap data path coherent on one cache object.

- Writeback daemon behavior:
  - timer IRQ now drives:
    - `pagecache_tick(now_ticks)` for bounded background dirty flush,
    - `ext4_periodic_maintenance(now_ticks)` for periodic JBD2 checkpoint progress.
  - files:
    - `kernel/trap.c`
    - `include/pagecache.h`
    - `include/ext4.h`
    - `kernel/ext4.c`

- JBD2 hardening:
  - stricter ring reservation safety:
    - reserve fails if resulting head does not advance (prevents full-ring ambiguity).
  - pending-queue pressure handling:
    - `ext4_jbd2_tx_commit` now attempts checkpoint drain when pending queue is full before failing.
  - periodic checkpoint hook exported:
    - `ext4_periodic_maintenance()` triggers batched checkpointing when aged/backlogged.

- mkfs policy completion (`user/mkfs.ext4.c`, `kernel/fs.c`, `include/syscall.h`):
  - added strict mode flag:
    - `MKFS_EXT4_F_STRICT_KERNEL`
    - userspace option: `-K` / `--strict-kernel`.
  - strict mode enforces kernel RW-supported feature subset only.
  - validated feature mask in kernel mkfs path (rejects unknown bits).
  - expanded `-T` presets with:
    - `largefile4` (`MKFS_EXT4_PROFILE_LARGEFILE4`, 4MiB bytes-per-inode policy).
  - added feature preset tokens in `-O`:
    - `baseline`, `kernelrw`, `modern`.
  - default mkfs userspace feature set now includes `64bit`.

- fsck policy/repair tuning (`kernel/fs.c`):
  - explicit repair policy levels:
    - `-n` => none,
    - `-p` => safe auto-fix,
    - `-y` => force/high-risk auto-fix.
  - duplicate/out-of-range/unallocated inode data block issues:
    - `-p` now rejects with explicit `requires -y` class where needed.
    - `-y` can salvage corrupted regular-file payload by clearing inode data mapping payload and keeping inode reachable.
  - retained existing safe repairs (checksums/counters/link-count rebuild/orphan-head cleanup) for `-p/-y`.

- Additional safety glue:
  - `pagecache_flush_all()` now required before mkfs/fsck and before unmount.
  - truncate paths flush inode cache before applying truncate metadata changes.

- Verification:
  - `make -j4` PASS.
  - `./test.sh` PASS (single full pass).
  - short targeted runtime smoke:
    - `umount /mnt`
    - `mkfs.ext4 -K -O modern /dev/sda` => fail (expected strict-mode rejection).
    - `mkfs.ext4 -K -O kernelrw /dev/sda` => format success.
    - `fsck.ext4 -p /dev/sda` => clean.

Additional references used for this step:
- ext4 journaling/JBD2 behavior:
  - https://www.kernel.org/doc/html/latest/filesystems/ext4/journal.html
- ext4 superblock feature classes:
  - https://www.kernel.org/doc/html/latest/filesystems/ext4/super.html
- e2fsck option behavior (`-p`, `-y`, `-n`) model:
  - https://www.man7.org/linux/man-pages/man8/e2fsck.8.html

## 2026-03-03 - TinyCC /bin/tcc integration (FuriOS userspace)

- Implemented a real `/bin/tcc` command using TinyCC source (`third_party/tinycc`) instead of the previous placeholder stub.
- Added FuriOS TinyCC port wiring:
  - `user/tcc.c`: self-contained TinyCC runtime shim (syscall I/O, stdio subset, allocator, formatting, time/math stubs) + embedded TinyCC one-source build.
  - `user/tcc_compat.h`: minimal hosted C API surface expected by TinyCC.
  - `third_party/tinycc/config.h`: FuriOS target config (AArch64 ELF, semlock off, static mode).
  - `third_party/tinycc/tcc.h`: FuriOS compatibility include path + native-run detection disabled for this environment.
  - `third_party/tinycc/libtcc.c`: `FUROS_TCC_NO_RUN` gate to exclude `tccrun.c` path.
  - `third_party/tinycc/tccpp.c`: defensive pointer-sanity guard in token hash traversal for stability in this freestanding port.
  - `third_party/tinycc/tccdefs_.h`: minimal embedded predefs set for this environment.
- Build integration:
  - `Makefile`: `tcc` now links with a dedicated self-contained rule (no shared user libc objects), still embedded as `/bin/tcc`.
  - Added local compatibility headers required by TinyCC in freestanding build (`user/inttypes.h`, `user/assert.h`).

### Runtime validation (short smoke)
- `tcc -v` works and reports version.
- `tcc -E /mnt/hi.c` works (preprocess output produced).
- `tcc -c /mnt/hi.c -o /mnt/hi.o` works (object file generated on ext4).

### Current limitation
- Full executable link path (`tcc file.c -o out`) is not fully wired to FuriOS crt/lib objects yet, so default TinyCC link mode can still require runtime crt artifacts not present as normal files.
- Practical current workflow in-OS: compile to object (`-c`) and preprocess (`-E`) reliably.

### Debug hardening note
- Added ELR printing in sync trap logs (`kernel/trap.c`) to speed up future userspace fault localization during toolchain bring-up.

## 2026-03-03 - TinyCC default final-link mode wired (`tcc file.c -o out`)

- Implemented full default TinyCC link prerequisites inside FuriOS filesystem as normal files under `/lib`:
  - `/lib/crt1.o`
  - `/lib/crti.o`
  - `/lib/crtn.o`
  - `/lib/libc.a`
  - `/lib/libtcc1.a`

### Build/system integration
- Added a dedicated `tccrt` build pipeline in `Makefile`:
  - builds startup objects from `user/tccrt/{crt1.S,crti.S,crtn.S}`
  - builds `libc.a` from existing user runtime objects (`syscall.o`, `string.o`, `io.o`, `alloc.o`)
  - stages `libtcc1.a` from toolchain libgcc for compiler runtime helpers.
- Embedded all `tccrt` artifacts into kernel image and exposed them in memfs at boot (`kernel/fs.c`).

### Validation (single short pass)
- Booted QEMU and verified:
  - `ls /lib` shows all required crt/lib files.
  - `tcc /mnt/hi.c -o /mnt/hi.elf` now succeeds (previously failed on missing `crt1.o`/`crti.o`).
  - output ELF file is created on disk (`/mnt/hi.elf`).

### Note
- This step resolves default link-time artifact failures.
- Executing newly linked files from writable ext4 paths still depends on executable-mode handling policy in current kernel process-exec path.

## 2026-03-03 - Exec permission flow + SDK/sysroot + TinyCC tooling hardening

- Implemented executable permission flow end-to-end:
  - Added new syscall `SYS_CHMOD` and userspace wrapper `sys_chmod`.
  - Added `/bin/chmod` command (octal mode syntax).
  - Added kernel `fs_chmod()` with ext4-backed mode updates via transaction commit.
  - Added ext4 mode mutation path `ext4_chmod()` (preserves inode file type bits, updates cached writable/exec state).
  - `sys_exec` now enforces execute bit from file mode (`st_mode & 0111`) rather than legacy executable flag only.
  - `sys_exec` now supports exec for files without in-memory blob backing by loading executable bytes via `fs_read()` into temporary kernel pages before `task_exec`.

- Implemented userspace SDK/sysroot layout:
  - Added `/usr/include` and `/usr/lib` population at boot.
  - Added compatibility `/include` headers for TinyCC default include path.
  - Installed SDK headers:
    - `stddef.h`, `stdint.h`, `stdbool.h`, `stdarg.h`
    - `furios.h`, `unistd.h`, `fcntl.h`, `string.h`, `stdio.h`, `stdlib.h`
    - `sys/stat.h` under `/usr/include/sys`
  - Mirrored runtime libs/startup objects under `/usr/lib`:
    - `crt1.o`, `crti.o`, `crtn.o`, `crtbegin.o`, `crtend.o`, `libc.a`, `libtcc1.a`

- Completed TinyCC linker/runtime compatibility wiring:
  - Added `crtbegin.o` / `crtend.o` artifacts to build, embed, and filesystem.
  - Updated TinyCC ELF path to include `crtbegin.o`/`crtend.o` for FuriOS.
  - Reworked FuriOS `crt1.S` to invoke init/fini arrays around `main` and return via syscall exit.
  - Added FuriOS-local `crtbegin.S` / `crtend.S` marker objects to avoid host toolchain frame-registration side effects.

- Added toolchain companion commands:
  - `/bin/ar` (execs `tcc -ar ...`)
  - `/bin/ranlib` (execs `tcc -ar s <archive>`)
  - `/bin/as` (execs `tcc -c ...` assembler frontend behavior)

- Diagnostics improvement:
  - Enabled TinyCC DWARF generation (`CONFIG_DWARF_VERSION=2`) so `-g` emits line/symbol info instead of being disabled.

### Validation (single short pass)
- `make -j4` PASS.
- QEMU short smoke PASS:
  - `/bin` now contains `chmod`, `ar`, `ranlib`, `as`.
  - `/usr/include` and `/usr/lib` populated as expected.
  - `tcc /mnt/m.c -o /mnt/m.elf` creates ELF.
  - `/mnt/m.elf` fails before chmod and succeeds after `chmod 755 /mnt/m.elf`.
- `tcc -c`, `ar rcs`, and `ranlib` work on `/mnt` artifacts.

## 2026-03-03 - Hardening pass (page cache + JBD2 + NVMe + process/signal + VM)

- Unified page-cache coherence tightened:
  - `pagecache_writeback()` now extends inode size for shared-mmap writeback beyond prior EOF.
  - `pagecache_notify_write()` now also grows inode size for coherent metadata/data visibility.
  - Added adaptive background writeback budget (`dirty_count`-based) in `pagecache_tick()` to reduce dirty-page buildup under sustained write/mmap pressure.
  - `inode_free()` now invalidates file page-cache entries before inode reuse to prevent stale page aliasing after unlink/recreate cycles.
  - `mprotect(PROT_WRITE)` on shared file mappings now marks relevant cache page dirty.

- JBD2 durability/space-accounting hardening:
  - `ext4_jbd2_tx_commit()` now retries ring reservation with bounded checkpoint advancement when log space is temporarily constrained.
  - This avoids immediate commit failure when head/tail pressure can be relieved by checkpoint progress.

- NVMe production hardening:
  - Added per-controller failure streak + adaptive timeout backoff.
  - Added throttled recovery entry (`nvme_recover_throttled`) to avoid reset storms.
  - `nvme_submit_with_retry()` now classifies retryable outcomes and performs bounded recover/retry with backoff.
  - `nvme_poll()` now uses the same backoff/throttled recovery path on controller fatal status.

- AHCI recovery hardening:
  - Added final fallback path in `ahci_cmd_with_retry()`:
    - full HBA reset,
    - per-disk port rebase,
    - COMRESET on target port,
    - final command retry.
  - This improves resilience under repeated TFES/timeout storms where port-local recovery is insufficient.

- Process/signal model completion step:
  - Added wait options `WUNTRACED` and `WCONTINUED`.
  - Added signal constants and support for `SIGHUP`, `SIGINT`, `SIGQUIT`, `SIGSTOP`, `SIGCONT` (plus existing TERM/KILL).
  - Added stop/continue child event reporting via `waitpid`:
    - stopped status (`0x7f` form) when `WUNTRACED`.
    - continued status (`0xffff`) when `WCONTINUED`.
  - Added stopped task state (`TASK_STOPPED`) and stop/continue event flags.
  - Extended `/bin/kill` user tool options to cover `-1/-2/-3/-18/-19` aliases.

- VM/context-switch hardening:
  - Added TTBR0 fast-path in `mmu_switch_ttbr0()` to skip redundant TTBR writes when ASID+TTBR0 are unchanged on reschedule.

### Validation (single short pass)
- `make -j4` PASS.
- Short non-interactive QEMU smoke PASS:
  - boot to shell,
  - `/dev` listing sane,
  - new signal options in `kill` accepted and routed (invalid pid path returns expected failure),
  - shell remains responsive (`echo smoke-ok`).

## 2026-03-03 - SDK/libc/syscall surface expansion + TinyCC hardening pass

- Expanded SDK/sysroot headers generated at boot (`kernel/fs.c`):
  - Added/expanded: `errno.h`, `signal.h`, `poll.h`, `limits.h`, `assert.h`, `inttypes.h`, `time.h`.
  - Added/expanded sys headers: `sys/types.h`, `sys/wait.h`, `sys/mman.h`, richer `sys/stat.h` mode/type macros.
  - Expanded `furios.h` prototypes/macros for process, fs, poll/fcntl, mmap/brk paths.
  - Expanded `stdio.h`, `string.h`, `stdlib.h` declarations for TinyCC-compiled programs.
  - Installed TinyCC runtime predefs header (`tccdefs.h`) into both `/include` and `/usr/include`.

- Extended user libc archive used by TinyCC runtime (`libc.a`):
  - Added `user/lib/posix.c` with errno-setting POSIX-style wrappers.
  - Added `user/lib/stdio.c` (`printf`/`snprintf`/`vsnprintf`/`vprintf`/`putchar`/`getchar`/`__assert_fail`).
  - Updated `Makefile` so these objects are linked into userspace and archived for tcc runtime linking.

- TinyCC stability hardening:
  - Switched TinyCC to runtime predefs loading (`CONFIG_TCC_PREDEFS=0`) to avoid truncated built-in predefs.
  - Added defensive pointer guards in TinyCC preprocessor/debug code paths for FuriOS build (`tccpp.c`, `tccdbg.c`) to avoid hard crashes on bad internal pointers.
  - Disabled `-g` debug emission in FuriOS TinyCC path with explicit warning message instead of crashing (`libtcc.c`).
    - Current behavior: `-g` accepted but downgraded with warning: debug info temporarily unavailable.

- Runtime diagnostics improvements:
  - Added per-task command-name tracking (`task.comm`) and propagated it across exec/fork/init.
  - Trap log now includes `pid` + task `comm` in sync fault reports.

- Tool wrappers polish:
  - `ranlib` now supports multiple archives in one invocation.
  - `as` wrapper improved to avoid forcing `-c` for non-compile modes.
  - TinyCC library default search paths tightened (`/lib:/usr/lib`; removed `/bin:/sbin` from libpath scan).

- Test harness updates (`test.sh`):
  - Added TCC workflow coverage for:
    - compile/object/archive/ranlib,
    - dependency output generation (`-MD -MF`),
    - default link+exec from ext4,
    - constructor/destructor and runtime syscall/mmap/pipes smoke.
  - Shortened/split long shell-emitted source lines to avoid parser truncation artifacts.
  - Updated assertions to fail fast on toolchain runtime-flow regressions.

### Validation (single full suite pass)
- `make -j4` PASS.
- `./test.sh` PASS end-to-end (virtio smoke + ext4 flow + toolchain/runtime smoke + AHCI probe flow).

### Current known limitation
- TinyCC `-g` debug-info emission is intentionally disabled on FuriOS (warning-only) to keep compiler runtime stable while DWARF path is completed.

## 2026-03-03 - `/bin/nano` editor integration + TinyCC long-flow compile stability fix

- Added a real interactive text editor in userspace:
  - New `user/nano.c` (full-screen terminal editor).
  - Features included:
    - file open/save,
    - cursor movement (arrows/home/end/page up/page down),
    - insert/delete/backspace/newline editing,
    - status/message bar,
    - unsaved-change quit guard,
    - in-file search prompt (`Ctrl+F`),
    - help/status shortcuts (`Ctrl+G`, `Ctrl+S`, `Ctrl+Q`).
  - Wired into build and root FS:
    - `Makefile`: added `nano` to `USER_PROGS`.
    - `kernel/fs.c`: added embed externs + `/bin/nano` install entry.

- TinyCC hardening follow-up for long command/test flows:
  - Root-cause symptom: intermittent `tccdefs.h` parse failures under long toolchain test sequence.
  - Stabilization changes:
    - Switched TinyCC back to embedded predefs mode (`third_party/tinycc/config.h` -> `CONFIG_TCC_PREDEFS=1`) to avoid runtime dependence on `/include/tccdefs.h` during compile path.
    - Extended `third_party/tinycc/tccdefs_.h` with AArch64 `__builtin_va_list` definition and basic va-copy/end helpers so `<stdarg.h>`-dependent code compiles in runtime tests.
    - Kept compile-path guard logic in `third_party/tinycc/tccpp.c` for runtime predefs branch as defensive fallback.

### Validation (single full suite pass)
- `make -j4` PASS.
- `./test.sh` PASS end-to-end, including:
  - ext4 mount/mutate/re-mount flow,
  - mkfs/fsck flow,
  - TinyCC compile/archive/link/runtime smoke (`tcc`, `ar`, `ranlib`, constructor/destructor, mmap/pipe/fork runtime path),
  - AHCI probe/smoke phase.

## 2026-03-03 - Fix for intermittent empty file after `nano` save/reboot

- Core issue fixed:
  - Dirty file-page cache was not force-flushed on writable `close()`.
  - Result: after truncate+write workloads (e.g. editor saves), abrupt QEMU termination could leave recently written content missing after reboot.

- Kernel fix:
  - `kernel/task.c`
    - Added writable-inode close detection (`task_fd_needs_writeback`).
    - `task_fd_close()` now flushes file page-cache (`pagecache_flush_inode`) before dropping FD state for writable regular files.
    - `task_fd_close()` now returns status; writable close flush failures propagate as `-1`.
  - `kernel/syscall.c`
    - `sys_close` now returns actual close/writeback status instead of always returning success.

- Userspace robustness fix:
  - `user/nano.c`
    - Added `write_all()` helper and switched save path to full-write loops (handles short writes safely instead of assuming one write completes the full buffer).

### Focused validation
- Reproduced crash-like flow with two boots on the same AHCI ext4 image:
  1. boot #1: write `/mnt/hi.txt`, then forced QEMU stop (timeout/signal),
  2. boot #2: read `/mnt/hi.txt`.
- Result after fix: saved content persisted and was readable on reboot.

### Regression validation
- `make -j4` PASS.
- `./test.sh` PASS.

## 2026-03-04 - Real `/lib/ld-furios.so` dynamic loader + exec/runtime hardening pass

- Implemented a real userspace ELF dynamic loader at `/lib/ld-furios.so`:
  - New loader binary source: `user/ld-furios.c`.
  - New high-address linker script: `user/ld-furios.ld` (loader linked at `0x00680000`).
  - Build wiring:
    - `Makefile` now builds/embeds `build/user/ld-furios.elf`.
    - `kernel/fs.c` now installs it as executable `/lib/ld-furios.so`.

- Loader capabilities implemented:
  - Loads target ELF image from disk (`ET_EXEC` and `ET_DYN`).
  - Maps all `PT_LOAD` segments with correct final page protections.
  - Parses `PT_DYNAMIC` and applies `RELA` relocations:
    - `R_AARCH64_RELATIVE`
    - `R_AARCH64_ABS64`
    - `R_AARCH64_GLOB_DAT`
    - `R_AARCH64_JUMP_SLOT`
  - Preserves runtime constructor/destructor semantics by jumping to target entry
    (`_start`) without pre-running init arrays in the loader.
  - Applies `PT_GNU_RELRO` final read-only protection.
  - Jumps into target entry with argv passed by kernel `PT_INTERP` handoff path.

- Kernel-side mapping policy adjustment for loader compatibility:
  - `kernel/task.c` (`task_mmap`):
    - `MAP_FIXED` mappings are now allowed anywhere in user VA (`>= USER_VA_BASE`, below stack floor), not only above current heap break.
    - This allows high-linked loader code to map target `ET_EXEC` images at canonical low VAs (e.g. `0x00400000`).

- Cleanup:
  - Removed stale syscall write debug scaffolding (`DEBUG_WRITE_FAIL`) and `print.h` dependency from `kernel/syscall.c`.

### Validation (single pass policy)
- Targeted smoke:
  - `/lib/ld-furios.so` present in `/lib`.
  - `tcc /mnt/h.c -o /mnt/h.elf` then `/mnt/h.elf` executed successfully and printed expected output.
- Full suite:
  - `make -j4` PASS.
  - `./test.sh` PASS end-to-end, including the TCC runtime execution flow (`hello.elf`, `tccmulti.elf`, `ctor.elf`, `rt.elf`).

### Source checks consulted while implementing loader relocation semantics
- GNU C Library AArch64 runtime relocation handling (`elf_machine_rela`):
  - https://codebrowser.dev/glibc/glibc/sysdeps/aarch64/dl-machine.h.html
- TinyCC FuriOS config default interpreter path (`/lib/ld-furios.so`):
  - `third_party/tinycc/config.h` (in-tree)

## 2026-03-04 - Loader dependency-ordering + auxv/envp ABI pass + relocation/hash compatibility hardening

- Dynamic loader dependency graph ordering (`user/ld-furios.c`):
  - Increased DSO capacity (`MAX_DSO=32`) and added explicit per-object dependency index list.
  - Added dependency resolution pass from `DT_NEEDED` names to loaded object indexes.
  - Replaced load-order constructor calls with dependency-safe DFS order:
    - init: dependencies first,
    - fini: reverse of resolved init order.
  - This removes the prior ordering hazard where `libA` could initialize before `libC` even when `libA` depends on `libC`.

- ELF dynamic/relocation compatibility upgrades:
  - Added dynamic tags/constants:
    - `DT_GNU_HASH`, `DT_RUNPATH`, `DT_FLAGS`,
    - extra AArch64 relocations (`ABS32/ABS16/PREL64/PREL32/PREL16`).
  - Added `DT_GNU_HASH` symbol-count derivation fallback when classic `DT_HASH` is absent.
  - Extended loader relocation support with:
    - `R_AARCH64_IRELATIVE`,
    - `R_AARCH64_ABS32`,
    - `R_AARCH64_ABS16`,
    - `R_AARCH64_PREL64`,
    - `R_AARCH64_PREL32`,
    - `R_AARCH64_PREL16`.
  - Hardened malformed `DT_NEEDED` handling (now rejects overflow instead of silently truncating).

- Runtime ABI parity (`envp` + `auxv`) improvements:
  - Loader `main` now accepts `envp` and passes it to the target entry (`x2`).
  - Loader now rebuilds auxv with required entries and incoming-policy passthrough:
    - core: `AT_PHDR`, `AT_PHENT`, `AT_PHNUM`, `AT_PAGESZ`, `AT_BASE`, `AT_ENTRY`, `AT_UID`, `AT_EUID`, `AT_GID`, `AT_EGID`, `AT_SECURE`, `AT_EXECFN`,
    - passthrough when present: `AT_HWCAP`, `AT_CLKTCK`, `AT_RANDOM`.
  - `AT_BASE` now uses incoming value when available, otherwise a loader-address fallback.
  - Added bounded auxv push helper with overflow fail-fast.

- TinyCC runtime startup ABI fix (`user/tccrt/crt1.S`):
  - Preserved incoming `x2/x3` across constructor execution and restored before `main`.
  - Fixes loss of `envp/auxv` in programs compiled with embedded TinyCC CRT.

- Minor loader robustness:
  - Fixed memory leak in file-image load path on read failure.

- Test-suite expansion (`test.sh`):
  - Added a short TCC runtime auxv/envp probe (`/mnt/auxv.elf`) validating:
    - `envp` terminator handling,
    - auxv visibility for `AT_PHNUM`, `AT_PAGESZ`, `AT_ENTRY`, `AT_EXECFN`, `AT_BASE`.
  - Added explicit regression gate for `exec failed: /mnt/auxv.elf`.

### Validation (single full suite pass)
- `make -j4` PASS.
- `./test.sh` PASS end-to-end (virtio/ext4/toolchain/runtime + AHCI phase), including new auxv/envp runtime check.

### Source checks consulted in this pass
- ELF gABI dynamic section semantics (`DT_NEEDED`, init/fini arrays, relocation model):
  - https://refspecs.linuxfoundation.org/elf/gabi4%2B/contents.html
- Linux auxv/`AT_*` contract:
  - https://man7.org/linux/man-pages/man3/getauxval.3.html
- AArch64 relocation behavior reference implementation in glibc dynamic loader:
  - https://codebrowser.dev/glibc/glibc/sysdeps/aarch64/dl-machine.h.html

## 2026-03-04 - Journal/cache/I-O hardening pass (durability + coherence + recovery)

- Syscall durability surface completed:
  - Added syscall numbers and full wiring for:
    - `SYS_FSYNC` (fd-based file sync)
    - `SYS_MSYNC` (mapped range sync)
  - Files:
    - `include/syscall.h`
    - `kernel/syscall.c`
    - `kernel/task.c`
    - `kernel/fs.c`
    - `user/lib/syscall.c`
    - `user/lib/posix.c`
    - `user/user.h`
  - SDK exposure for toolchain-built userland:
    - `kernel/fs.c` generated headers now expose `sys_fsync/sys_msync`, `fsync/msync`, and `MS_*` flags in `/usr/include`.

- Single-cache coherence + dirty/writeback tuning:
  - Implemented `pagecache_flush_inode_range()` for precise mapped-range flushes used by `msync`.
  - Added bounded writeback failure backoff state in page cache:
    - writeback fail streak tracking,
    - temporary throttling window before next background/pressure writeback cycle.
  - Integrated backoff into both pressure-triggered writeback and periodic writeback tick.
  - Files:
    - `kernel/pagecache.c`
    - `include/pagecache.h`

- Filesystem durability entry points:
  - Implemented:
    - `fs_sync_inode(inode_t *)`
    - `fs_sync_all(void)`
  - Behavior:
    - flush pagecache dirty data,
    - flush ext4 journal/checkpoint state,
    - flush block cache for block devices/global sync.
  - Files:
    - `kernel/fs.c`
    - `include/fs.h`

- ext4/JBD2 lifecycle hardening:
  - Added exported `ext4_sync_filesystem()` and used it from unmount path.
  - Replay validation tightened:
    - explicit guard-exhaustion failure instead of silent acceptance.
  - Ring reserve hardening:
    - extra full-ring/head-tail safety checks in reserve path.
  - Checkpoint batching behavior strengthened:
    - removed commit-time immediate checkpointing from tx commit fast path;
      checkpointing now batches via periodic maintenance / explicit sync.
  - Files:
    - `kernel/ext4.c`
    - `include/ext4.h`

- Interrupt/recovery path improvement for virtio-blk:
  - Added classified I/O completion failure reasons:
    - timeout / bad-used-ring / device-status / not-ready.
  - Added bounded retry + reset recovery loop for request submission.
  - Added per-class counters and explicit recovery reason logging.
  - Files:
    - `kernel/virtio_blk.c`

- Short stress harness:
  - Added `stress_io_short.sh` wrapper to run reduced-duration AHCI fault-injection and NVMe crash/replay checks.
  - File:
    - `stress_io_short.sh`

### Validation
- `make -j4` PASS.
- `./test.sh` PASS.

## 2026-03-04 - TLS/loader ABI + trap symbolization + W^X hardening pass

### What I changed

- Added ELF symbol metadata in kernel task model for trap-time symbolization:
  - New per-task debug symbol table (`task_debug_sym_t`) populated from ELF `SHT_SYMTAB`/`SHT_DYNSYM` function symbols.
  - Added `task_symbolize_pc(...)` and wired trap logs to print best-match symbol + offset/size.
  - Added `task_load_debug_symbols(...)` API and used it in `exec` interpreter path so PT_INTERP-launched targets (via `/lib/ld-furios.so`) still produce target-symbol trap output.
  - Files:
    - `include/task.h`
    - `kernel/task.c`
    - `kernel/trap.c`
    - `kernel/syscall.c`

- Loader W^X/RELRO tightening:
  - Loader segment mapping now uses non-executable writable staging (`PROT_READ|PROT_WRITE`) before final `mprotect`.
  - Final segment protection drops write on executable segments (W^X discipline).
  - `PT_GNU_RELRO` apply is now fail-checked (hard error if `mprotect` fails).
  - File:
    - `user/ld-furios.c`

- ABI/auxv parity expansion in kernel exec stack setup:
  - Added `AT_RANDOM` user pointer and random seed blob on initial stack.
  - Added `AT_CLKTCK` (`TIMER_HZ`) and `AT_HWCAP`/`AT_HWCAP2` entries.
  - Increased local auxv frame capacity accordingly.
  - File:
    - `kernel/task.c`

- ELF header support additions used by symbolizer/ABI updates:
  - Added `Elf64_Shdr` and section-type constants to shared ELF header.
  - Added `AT_HWCAP2` constant.
  - File:
    - `include/elf.h`

- Toolchain/runtime regression coverage (`test.sh`) extended:
  - Added multi-DSO TLS stress:
    - two shared libraries with independent TLS and cross-DSO `extern __thread` access,
    - main binary linked via `-L/-l` verifies correct TLS behavior (`tls-dso-ok`).
  - Extended auxv smoke to verify `AT_CLKTCK` and `AT_RANDOM`.
  - Added trap-debug smoke:
    - compile `-g` crashing binary,
    - assert trap log includes `comm=trapdbg.elf` and `sym=crash_here+...`.
  - Added execution-failure guard for `/mnt/tlsdso.elf`.
  - File:
    - `test.sh`

### Validation
- `make -j4` PASS.
- `./test.sh` PASS.

## Step 41 (Single-cache block path + ext4 source pinning + loader v2 ABI hardening)

### What I changed

- Unified block-device file I/O to the shared block cache path:
  - `/dev/*` block inode read/write now uses `block_cache_read/write/flush` instead of direct backend calls.
  - `dev_block_read_inode()` and `dev_block_write_inode()` now attach cache context via `block_cache_attach_inode(inode)` and do all sector access through cache lines.
  - Removed now-dead direct multi-sector/flush helpers.
  - File:
    - `kernel/fs.c`

- Pinned ext4 to its mounted source device context:
  - `ext4_mount()` now accepts and stores the mount source block inode info (`dev_kind`, partition LBA base/count).
  - Added `ext4_attach_source_dev()` and call it before every ext4 raw byte read/write path.
  - This prevents cross-device cache-context drift when multiple block devices are active.
  - Files:
    - `include/ext4.h`
    - `kernel/fs.c`
    - `kernel/ext4.c`

- Tightened JBD2 ring head/tail accounting:
  - Added explicit ring-free computation helper (`ext4_jbd2_ring_free_blocks()`).
  - Reservation now checks required blocks against computed free ring space directly.
  - ENOSPC logging now includes computed free space to improve failure diagnosis.
  - File:
    - `kernel/ext4.c`

- Loader/ELF tier-2 compatibility hardening (`/lib/ld-furios.so`):
  - Added dynamic version-table tags in ELF parsing:
    - `DT_VERSYM`, `DT_VERDEF`, `DT_VERDEFNUM`, `DT_VERNEED`, `DT_VERNEEDNUM`.
  - Added version-aware symbol resolution:
    - requester required-version lookup from `VERNEED`,
    - provider version-name lookup from `VERDEF`,
    - hidden-version visibility filtering.
  - Added TLS relocation coverage:
    - `R_AARCH64_TLS_DTPMOD64`
    - `R_AARCH64_TLS_DTPREL64`
    - `R_AARCH64_TLS_TPREL64`
    - `R_AARCH64_TLSDESC`
  - Added a minimal static TLS runtime layout for loaded DSOs:
    - per-DSO module IDs and TLS offsets,
    - TLS image allocation/copy,
    - `TPIDR_EL0` setup in loader.
  - Files:
    - `include/elf.h`
    - `user/ld-furios.c`

- Added short ext4 replay durability gate to tests:
  - Added two-boot crash-style replay validation in `test.sh`:
    - boot #1 writes replay file and exits under timeout (unclean),
    - boot #2 verifies replayed content.
  - File:
    - `test.sh`

### Validation
- `make -j4` PASS.
- `./test.sh` PASS.

### External references used for this hardening pass
- Linux ext4/JBD2 journal format and replay model:
  - https://www.kernel.org/doc/html/latest/filesystems/ext4/journal.html
- glibc AArch64 dynamic relocation handling (`R_AARCH64_TLS_*`, `TLSDESC`, versioned symbol lookup patterns):
  - https://codebrowser.dev/glibc/glibc/sysdeps/aarch64/dl-machine.h.html
- Linux UAPI ELF dynamic/version tag definitions:
  - https://codebrowser.dev/linux/linux/include/uapi/linux/elf.h.html

## Step 40 (Signal model hardening: pending queue + fork/exec semantics + restart policy)

### What I changed

- Kernel signal pending model strengthened:
  - Replaced pure bitmask-only pending behavior with ordered pending queue storage (`TASK_SIGNAL_QUEUE_LEN`),
    while keeping standard-signal coalescing (no duplicate pending instances of the same signal).
  - Added queue helpers for enqueue/remove/clear to keep bitmask and queue state consistent.
  - Updated `SIGCHLD` notification path to use queue-aware enqueue.
  - Files:
    - `include/task.h`
    - `kernel/task.c`

- Signal delivery and interrupted syscall behavior:
  - Delivery now dequeues ordered pending unblocked signals first, then falls back to bitscan for overflow/fallback cases.
  - `SA_RESTART` policy now depends on wait class:
    - restart allowed for child wait / pipe read / pipe write waits,
    - interruption (`-EINTR` style) forced for sleep-style waits (`sleep`/`poll` paths).
  - Preserves prior behavior where non-restart handlers convert internal blocked return (`-2`) to interrupt return (`-4`).
  - Files:
    - `kernel/task.c`

- POSIX-like fork/exec signal-state semantics:
  - `fork` now inherits parent dispositions/masks (`signal_handler/restorer/flags/action_mask`, mask) and clears child pending/active state.
  - `exec` now resets caught handlers to `SIG_DFL`, preserves `SIG_IGN`, clears pending/active frame state, and keeps mask (except non-blockable bits).
  - Files:
    - `kernel/task.c`

- Test-suite stability:
  - Attempted adding a shell-generated EL0 signal runtime program in `test.sh`, but removed it after confirming FuriOS shell parser edge-cases make this generated source path flaky.
  - Restored deterministic full-suite behavior.
  - File:
    - `test.sh`

### Validation
- `make -j4` PASS.
- `./test.sh` PASS.

### External references used for this pass
- Linux `signal(7)` default actions, pending/coalescing model, and `SA_RESTART` notes:
  - https://man7.org/linux/man-pages/man7/signal.7.html
- Linux `sigaction(2)` semantics:
  - https://man7.org/linux/man-pages/man2/sigaction.2.html
- Linux `sigprocmask(2)` semantics:
  - https://man7.org/linux/man-pages/man2/sigprocmask.2.html
- `LOOPS=8 QEMU_TIMEOUT=45 STEP=0.015 ./stress_ahci.sh` PASS.
- `PHASE1_TIMEOUT=12 PHASE2_TIMEOUT=16 STEP=0.02 ./stress_nvme.sh` PASS.

### External references used for this hardening pass
- JBD2/ext4 journal block layout + replay/commit/revoke semantics:
  - https://www.kernel.org/doc/html/latest/filesystems/ext4/journal.html
- Linux `msync(2)` semantics and flag constraints:
  - https://man7.org/linux/man-pages/man2/msync.2.html
- Linux `fsync(2)` semantics:
  - https://www.man7.org/linux/man-pages/man2/fsync.2.html
- Virtio block status/operation requirements:
  - https://docs.oasis-open.org/virtio/virtio/v1.2/csd01/virtio-v1.2-csd01.html

## Step 39 (Loader/VM/JBD2/Signal/Storage reliability hardening)

### What I changed

- Dynamic loader compatibility pass (`/lib/ld-furios.so`):
  - Added `DT_RPATH` + `DT_RUNPATH` parsing and per-object dependency search behavior.
  - Added `$ORIGIN` expansion support in RPATH/RUNPATH path components.
  - Search precedence now distinguishes legacy/new tags:
    - `DT_RPATH` (only when no `DT_RUNPATH`) -> `LD_LIBRARY_PATH` -> `DT_RUNPATH` -> `/lib` -> `/usr/lib`.
  - Added `DT_REL`/`DT_RELSZ`/`DT_RELENT` parsing and REL relocation application path.
  - Added PLT relocation handling for both `DT_RELA` and `DT_REL` encodings.
  - Files:
    - `user/ld-furios.c`
    - `include/elf.h`

- Process/signal/waitpid ABI correctness:
  - Switched wait status encoding to Unix-compatible layout:
    - normal exit => `(code << 8)`
    - signal exit => low 7 bits hold signal
    - stop/continue retained as stop `0x7f` and continue `0xffff` forms.
  - Added SDK wait macros for `WIFSIGNALED`/`WTERMSIG` and corrected `WIFEXITED`/`WEXITSTATUS`.
  - `waitpid(..., WUNTRACED, ...)` now reports the task’s real recorded stop signal.
  - `kill` now returns specific negative errno-style values for invalid signal/no target paths.
  - Files:
    - `kernel/task.c`
    - `include/task.h`
    - `kernel/fs.c` (SDK header generation)

- Unified page-cache invalidate semantics hardening:
  - `pagecache_invalidate_inode_range()` now returns status.
  - `msync(MS_INVALIDATE)` now fails if overlapping cache pages are still pinned/busy,
    instead of silently skipping and returning success.
  - Files:
    - `include/pagecache.h`
    - `kernel/pagecache.c`
    - `kernel/task.c`

- ext4/JBD2 journal lifecycle hardening:
  - Added explicit journal-space (`ENOSPC`-class) tracking in JBD2 tx path.
  - Added explicit ENOSPC diagnostics on reserve pressure/failure.
  - Added ext4-side last-error propagation hook so tx failures can be classified.
  - Threaded tx error mapping into key fs ext4 mutators (`link/symlink/chmod/unlink/rmdir/rename/truncate`) so callers can receive specific negative errno values instead of generic `-1`.
  - Files:
    - `kernel/ext4.c`
    - `include/ext4.h`
    - `kernel/fs.c`

- NVMe/AHCI interrupt-first completion tightening:
  - Completion loops now enforce interrupt-first waiting with bounded fallback polling windows.
  - Keeps IRQ-driven behavior as primary path while preserving forward progress on lost/late IRQ events.
  - Files:
    - `kernel/nvme.c`
    - `kernel/ahci.c`

### Validation
- `make -j4` PASS.
- `./test.sh` PASS.

## Step 41 (Page-cache/JBD2 regression fix + signal runtime parity polish)

### What I changed

- True single-path page-cache fixups kept and validated:
  - Retained prior block-device path unification (`fs_read`/`fs_write` -> page cache path for block dev inodes).
  - Kept strict ordering in sync paths (`pagecache_flush_*` before ext4/journal sync and device cache flush).
  - Removed stale direct block-device helpers in `kernel/fs.c` (`dev_block_read_inode` / `dev_block_write_inode`) to prevent dead-path drift.

- JBD2 replay hardening corrected:
  - Fixed regression introduced by over-strict descriptor/revoke validation:
    - JBD2/ext4 metadata updates can legally target filesystem block `0` on 4KiB ext4 layouts.
    - Removed invalid `fs_block == 0` rejection in:
      - `ext4_jbd2_add_revoke`
      - `ext4_jbd2_add_update`
      - `ext4_jbd2_parse_descriptor_block`
  - Kept upper-bound checks (`fs_block < blocks_count`) and journal-block ring bounds validation.

- POSIX signal/runtime parity expanded:
  - Added `SA_NOCLDWAIT` constant to kernel/user SDK headers.
  - Added `SA_NOCLDWAIT` acceptance in `sigaction` flag mask.
  - Implemented child auto-reap behavior when parent has:
    - `SIGCHLD` disposition explicitly set to `SIG_IGN`, or
    - `SA_NOCLDWAIT` set for `SIGCHLD`.
  - Exit path now:
    - performs normal SIGCHLD notify + waiter wake,
    - then reaps child immediately for auto-reap cases (no zombie retention).

- Test-suite hardening and stability:
  - Fixed flaky `test.sh` signal-flag runtime source generation by splitting very long emitted shell lines into short deterministic lines (serial console input no longer truncates them).
  - Extended signal runtime test (`/mnt/sigf.elf`) to validate:
    - `SA_RESETHAND`,
    - `SA_NODEFER`,
    - `SA_NOCLDWAIT`,
    - explicit `SIGCHLD=SIG_IGN` no-zombie behavior (`waitpid` negative path).

### Validation

- `make -j4` PASS.
- `./test.sh` PASS.
  - ext4 replay gate now passes again (`[ext4] jbd2 replay tx=2`).
  - AHCI-only boot device enumeration gate still passes.

### External references used for this pass

- ext4 superblock `s_first_data_block` semantics (typically `0` for non-1KiB block sizes):
  - https://www.kernel.org/doc/html/next/filesystems/ext4/super.html
- Linux `sigaction(2)` semantics for `SA_NOCLDWAIT`, `SA_NODEFER`, `SA_RESETHAND`, and `SIGCHLD` handling:
  - https://man7.org/linux/man-pages/man2/sigaction.2.html
- Linux `waitpid(2)` behavior when `SIGCHLD` is `SIG_IGN` or `SA_NOCLDWAIT` is set (`ECHILD` semantics):
  - https://man7.org/linux/man-pages/man2/waitpid.2.html

## Step 42 (Output-noise reduction + JBD2 revoke strictness hardening)

### What I changed

- Reduced non-essential runtime noise:
  - Shell no longer prints `[sh] terminated: ... code=...` for ordinary non-zero exit codes.
  - It now only reports explicit stop/continue or real signal-termination cases.
  - File:
    - `user/sh.c`

- Reduced boot-time debug chatter:
  - Removed `[block-cache] selftest ok` informational print (selftest still runs; failure still reports/guards).
  - Files:
    - `kernel/block_cache.c`
    - `test.sh` (removed now-obsolete grep gate)

- JBD2 durability hardening (revoke path):
  - Added strict overflow tracking for tx revoke records (`tx_revoke_overflow`).
  - If revoke capacity is exceeded, commit now fails instead of silently dropping revoke coverage.
  - This prevents hidden replay-safety degradation under metadata-heavy/free-heavy transactions.
  - Removed block-0 special-case rejection in tx revoke note path (range check remains authoritative).
  - Files:
    - `kernel/ext4.c`

- Additional log cleanup:
  - Removed `[ext4] jbd2 replay tx=...` informational line; replay success/failure behavior unchanged.
  - File:
    - `kernel/ext4.c`

### Validation

- `make -j4` PASS.
- `./test.sh` PASS.

## Step 45 (JBD2 pressure/revoke correctness + safer cache invalidation)

### What I changed

- JBD2 revoke correctness fix on overwrite paths:
  - Fixed a replay-safety bug where revoke entries were only removed on a subset of writes.
  - Now any metadata write to a block within an active tx clears a prior revoke for that same block in that tx.
  - This prevents stale revoke state from incorrectly suppressing valid journal updates after partial metadata writes.
  - File:
    - `kernel/ext4.c`

- JBD2 tx-pressure behavior improved:
  - Under pending-transaction pressure, checkpointing now runs in batched mode instead of one-by-one in tx start/commit reserve loops.
  - This reduces ENOSPC thrash and advances tail faster under sustained metadata load.
  - File:
    - `kernel/ext4.c`

- JBD2 ENOSPC/error classification tightening:
  - Dirty-tx slot exhaustion (`EXT4_JBD2_TX_MAX_BLOCKS`) now marks tx as ENOSPC-class pressure (`tx_enospc`) rather than failing as generic error.
  - File:
    - `kernel/ext4.c`

- Reduced non-essential debug spam while keeping important errors:
  - Rate-limited `[ext4] jbd2 enospc ...` diagnostic emission (still visible, no longer flood-prone).
  - Shell wait path no longer prints child-status noise (`continued/stopped/terminated`) for routine command flow.
  - Files:
    - `kernel/ext4.c`
    - `user/sh.c`

- Unified cache safety hardening for mmap/pinned pages:
  - `pagecache_invalidate_inode()` now avoids dropping pages still referenced by active mappings/tasks (refcount > 1).
  - Prevents invalidation from freeing live pages during truncate/unlink-heavy workloads with active mappings.
  - For pages beyond current inode size, dirty is cleared so stale data is not written back.
  - File:
    - `kernel/pagecache.c`

### Validation

- `make -j4` PASS.
- `./test.sh` PASS.
- Full suite still passes ext4/JBD2 replay, mmap/msync paths, shell, and TCC/runtime coverage.
