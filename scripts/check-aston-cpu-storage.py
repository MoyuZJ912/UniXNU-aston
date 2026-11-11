#!/usr/bin/env python3
"""Native host fixtures for the patched CPU policy and Data publication.

--xnu is XNU after all selected patches. --ramblock is the original pinned
checkout. API stand-ins exercise source decisions, not APFS or phone hardware.
Example: --cc /path/to/zig cc (or --cc clang).
"""
import argparse
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import tomllib

ROOT = Path(__file__).resolve().parents[1]


def function(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    while ';' in text[start:brace]:  # Skip a forward declaration.
        start = text.index(signature, start + len(signature))
        brace = text.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def run_host(cc, work, name, code, cpp=False):
    if sys.platform == 'win32' and Path(cc[0]).stem == 'zig':
        cc = [*cc, '-target', 'x86_64-windows-gnu']
    source = work / (name + ('.cpp' if cpp else '.c'))
    binary = work / (name + '.exe')
    source.write_text(code, encoding='utf-8')
    subprocess.run([*cc, '-std=c++17' if cpp else '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-pointer-bool-conversion', '-I', str(ROOT / 'package'), str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--xnu', required=True, type=Path)
    parser.add_argument('--ramblock', required=True, type=Path)
    parser.add_argument('--cc', nargs='+', default=['clang'])
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location('stage_ram', ROOT / 'scripts/prepare-aston-ramblock.py')
    stage = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(stage)
    board = tomllib.loads((ROOT / 'board.toml').read_text())
    assert 'armcpu-edge=1' in board['payload']['kernel_args']
    assert 'sheng-cpufreq-boost-ms=80' in board['payload']['kernel_args']
    assert 'sheng-cpufreq-frame-ms=0' in board['payload']['kernel_args']
    archives = board['kernel']['archives']
    assert archives.index('prepare-aston-ramblock.py') < min(archives.index('component:ShengUfs'), archives.index('component:RamBlock'))
    before = (args.ramblock / 'NativeRamBlock.cpp').read_bytes()
    with tempfile.TemporaryDirectory(prefix='aston-check-') as td:
        work = Path(td)
        (work / 'drivers').mkdir()
        (work / 'drivers/RamBlock').symlink_to(args.ramblock.resolve(), target_is_directory=True)
        staged = stage.stage(work)
        code = (staged / 'NativeRamBlock.cpp').read_text()
        assert stage.stage(work) == staged  # repeat without patching the shared source
        assert (staged / 'NativeRamBlock.cpp').read_text() == code
        assert (args.ramblock / 'NativeRamBlock.cpp').read_bytes() == before
        assert 'ram->attachStore();' not in function(code, 'static int selectVolumeSeed(')
        start = function(code, 'bool NativeRamBlock::start(')
        assert 'return publishStorage();' in start and 'registerService();' not in start
        publication = function(code, 'bool publishStorage()')
        run_host(args.cc, work, 'publication', r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
static unsigned mode, scenario, native_data_persist_state, native_data_persist_header, native_ram_block_ready;
static unsigned native_ufs_store_mode() { return mode; }
#define IOLog(...) ((void)printf(__VA_ARGS__))
struct Disk {
    bool direct=false, isBig=false, online=false, attached=false, published=false;
    unsigned checkpoint=1, observed=0;
    void attachStore() {
        assert(!online && !published); attached=true;
        if (!mode) return;
        if (scenario == 0) { direct=mode==2; checkpoint=9; native_data_persist_state=2; }
        else if (scenario == 1) { direct=mode==2; checkpoint=1; native_data_persist_state=3; }
        else { native_data_persist_state=scenario==2?7:4; }
    }
    void registerService() {
        assert(attached && online && native_ram_block_ready);
        published=true; observed=checkpoint; // APFS's first checkpoint read
    }
''' + publication + r'''
};
int main() {
    for (mode=0;mode<3;mode++) for (scenario=0;scenario<5;scenario++) for (unsigned big=0;big<2;big++) {
        native_ram_block_ready=0; native_data_persist_state=0; Disk d; d.isBig=big;
        bool allowed = mode==0 ? !big : scenario>=3 ? false : mode==2 ? scenario<2 : !big;
        assert(d.publishStorage()==allowed);
        assert(d.published==allowed && d.online==allowed && !!native_ram_block_ready==allowed);
        if (allowed) assert(d.observed == (mode && scenario==0 ? 9u : 1u));
    }
    puts("PASS: 30 publication cases; resumed checkpoint visible before APFS probe; unavailable writable store stays unpublished");
}
''', cpp=True)
        run_host(args.cc, work, 'cpu_samples', r'''
#include <assert.h>
#include <stdio.h>
#include "CpuFreqPolicy.h"
int main(void) {
    assert(cpufreq_busy_per_mille(100,0,100)==0);
    assert(cpufreq_busy_per_mille(100,100,100)==1000);
    assert(cpufreq_busy_per_mille(150,100,100)==500);
    assert(cpufreq_busy_per_mille(UINT64_MAX,100,100)==0);
    assert(cpufreq_busy_per_mille(100,UINT64_MAX,100)==0);
    assert(cpufreq_busy_per_mille(0,100,100)==0);
    assert(cpufreq_busy_per_mille(101,0,100)==0);
    assert(cpufreq_busy_per_mille(100,100,0)==0);
    assert(cpufreq_busy_per_mille(100,100,UINT64_MAX)<=1000);
    puts("PASS: CPU idle/busy, offline/rejoin, reset, zero and long sampling intervals");
}
''')
        scheduler = (args.xnu / 'osfmk/kern/sched_clutch.c').read_text()
        init = function(scheduler, 'static void\nsched_edge_cpu_init_completed(void)')
        assert 'sched_edge_ui_bucket_stays_on_p' not in scheduler
        assert 'assert(sched_edge_config_verify());' in init
        policy = init[init.index('sched_clutch_edge edge_config_defaults'):init.index('\n\tspl_t s')]
        verify = function(scheduler, 'static inline bool\nsched_edge_config_verify_non_decreasing_qos_strictness(')
        transitive = function(scheduler, 'static bool\nsched_edge_config_verify_transitive_traverse(')
        # The aston psets: 0 the A510 cluster (E), 1 the A715/A710 cluster and 2 the X3 core (both P).
        run_host(args.cc, work, 'edge', r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef unsigned pset_id_t;
typedef unsigned sched_bucket_t;
typedef uint64_t bitmap_t;
typedef struct {unsigned sce_migration_weight, sce_migration_allowed, sce_steal_allowed;} sched_clutch_edge;
enum {PSET_AMP_E, PSET_AMP_P, MAX_PSET_TYPES, TH_BUCKET_SCHED_MAX=6, PERMISSIVE_MIGRATION_BUCKET=0};
enum {kPsets=3};
static const unsigned pset_type[kPsets]={PSET_AMP_E,PSET_AMP_P,PSET_AMP_P};
static const unsigned sched_num_psets=kPsets;
static sched_clutch_edge matrix[kPsets][kPsets][6];
static sched_clutch_edge sched_edge_config_get(unsigned s,unsigned d,unsigned q) {return matrix[s][d][q];}
static bool bitmap_test(const bitmap_t *map,unsigned i) {return (map[0]>>i)&1;}
static void bitmap_set(bitmap_t *map,unsigned i) {map[0]|=1ull<<i;}
#define kprintf(...) ((void)0)
''' + verify + '\n' + transitive + r'''
static void fill(sched_clutch_edge defaults[MAX_PSET_TYPES][MAX_PSET_TYPES]) {
    for(unsigned s=0;s<kPsets;s++) for(unsigned d=0;d<kPsets;d++) for(unsigned q=0;q<6;q++)
        matrix[s][d][q]=defaults[pset_type[s]][pset_type[d]];
}
// sched_edge_config_verify() over the board's psets: what the DEVELOPMENT kernel asserts once the CPUs are up.
static bool verify_all(void) {
    bool pass=true;
    for(unsigned s=0;s<kPsets;s++) for(unsigned d=0;d<kPsets;d++) if(s!=d) for(unsigned q=0;q<6;q++)
        pass=sched_edge_config_verify_non_decreasing_qos_strictness(s,d,q)&&pass;
    for(unsigned d=0;d<kPsets;d++) for(unsigned q=0;q<6;q++) {
        bitmap_t visited[1]={0};
        pass=sched_edge_config_verify_transitive_traverse(d,d,q,visited)&&pass;
    }
    return pass;
}
int main(void) {
''' + policy + r'''
    fill(edge_config_defaults);
    assert(verify_all());
    for(unsigned q=0;q<6;q++) {
        assert(matrix[1][0][q].sce_migration_allowed && matrix[2][0][q].sce_migration_allowed);   // P -> E spills
        assert(matrix[0][1][q].sce_migration_allowed && matrix[0][1][q].sce_steal_allowed);       // E -> P spills (aston)
        assert(matrix[0][2][q].sce_migration_allowed && matrix[1][2][q].sce_migration_allowed);
    }
    // The previous policy prohibited the three high QoS buckets only. It must fail XNU's actual validator.
    for(unsigned q=0;q<3;q++) matrix[1][0][q]=no_spill;
    assert(!sched_edge_config_verify_non_decreasing_qos_strictness(1,0,3));
    // A steal-only E -> P edge also passes: migration-free edges add no paths for the transitive check.
    fill(edge_config_defaults);
    for(unsigned d=1;d<kPsets;d++) for(unsigned q=0;q<6;q++) matrix[0][d][q]=(sched_clutch_edge){0,0,1};
    assert(verify_all());
    // An E -> P spill to the X3 alone breaks transitivity (E reaches X3 through the A715 cluster's edge only).
    fill(edge_config_defaults);
    for(unsigned q=0;q<6;q++) matrix[0][1][q]=no_spill;
    assert(!verify_all());
    puts("PASS: XNU's QoS monotonicity and transitivity validators accept the E <-> P spill matrix on E, P, P psets");
}
''')
        processor = (args.xnu / 'osfmk/kern/processor.c').read_text()
        assert 'PROCESSOR_OFF_LINE' in function(processor, 'sheng_cpu_idle_mach(int cpu)\n{')
        capacity = function((args.xnu / 'bsd/kern/kern_exec.c').read_text(), 'static void\naston_data_capacity_checkpoint(')
        run_host(args.cc, work, 'capacity', r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
typedef void *mount_t;
struct vfs_attr {uint64_t f_bsize,f_blocks,f_bfree,f_bavail;int supported;};
#define VFSATTR_INIT(p) ((void)(p))
#define VFSATTR_WANTED(p,f) ((void)(p))
#define VFSATTR_ALL_SUPPORTED(p) ((p)->supported)
static int scenario, aston_data_capacity_status;
static uint64_t aston_data_capacity_bytes,aston_data_free_bytes,aston_data_available_bytes;
static void *vfs_context_current(void) {return 0;}
static int vfs_getattr(mount_t mp,struct vfs_attr *v,void *ctx) {
    (void)mp;(void)ctx;*v=(struct vfs_attr){4096,2097152,1048576,1048575,1};
    if(scenario==1)return EIO;
    if(scenario==2)v->supported=0;
    if(scenario==3)v->f_bsize=0;
    if(scenario==4)v->f_blocks=UINT64_MAX;
    if(scenario==5)v->f_bfree=v->f_blocks+1;
    if(scenario==6)v->f_bavail=v->f_bfree+1;
    return 0;
}
''' + capacity + r'''
int main(void) {
    const int expected[]={0,EIO,ENOTSUP,EINVAL,EINVAL,EINVAL,EINVAL};
    for(scenario=0;scenario<7;scenario++) {
        aston_data_capacity_bytes=aston_data_free_bytes=aston_data_available_bytes=0;
        aston_data_capacity_checkpoint(0);
        assert(aston_data_capacity_status==expected[scenario]);
        assert(aston_data_capacity_bytes==(scenario?0:UINT64_C(8589934592)));
    }
    puts("PASS: APFS capacity checkpoint preserves 64-bit sizes and reports query/geometry failures without fabricated free space");
}
''')
    print('PASS: staging is repeatable; upstream source unchanged; build order and boot arguments checked')


if __name__ == '__main__':
    main()
