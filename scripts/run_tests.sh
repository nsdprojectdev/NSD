#!/bin/bash
# NSD v1 Test Suite
# Usage: sudo ./scripts/run_tests.sh [ssd|hdd|hdd_deep|all]

set -e
NSD_SYSFS="/sys/kernel/nsd"

fio_run() {
    local name=$1 file=$2 rw=$3 bs=$4 direct=$5 runtime=$6 extra=$7
    fio --name="$name" --filename="$file" \
        --rw="$rw" --bs="$bs" --iodepth=1 --direct="$direct" \
        --size=1G --numjobs=1 --runtime="$runtime" --time_based \
        $extra --output-format=json 2>/dev/null | python3 -c "
import sys,json
d=json.load(sys.stdin)['jobs'][0]['read']
print(f'{d[\"bw_bytes\"]/1024/1024:.1f} MB/s  {d[\"iops\"]:.0f} IOPS  {d[\"clat_ns\"][\"mean\"]/1000:.0f}us')
"
}

run_test() {
    local desc=$1; shift
    echo "--- $desc ---"
    for state in "ON" "OFF"; do
        [ "$state" = "ON" ] && echo 0 > "$NSD_SYSFS/observe_only" || echo 1 > "$NSD_SYSFS/observe_only"
        for i in 1 2; do
            echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 2
            printf "  %s #%d: " "$state" "$i"
            fio_run "$@"
        done
    done
    echo 0 > "$NSD_SYSFS/observe_only"
}

[ "$EUID" -ne 0 ] && { echo "sudo required"; exit 1; }
lsmod | grep -q nsd || { modprobe nsd 2>/dev/null || insmod "$(dirname $0)/../nsd.ko" 2>/dev/null || { echo "NSD not loaded"; exit 1; }; }

echo "NSD v1 Tests"
cat "$NSD_SYSFS/stats" 2>/dev/null | grep -E "version:|dev_class:|thresh:|region_kb:|span_kb:"

case "${1:-all}" in
ssd)
    echo 0 > "$NSD_SYSFS/observe_only"
    run_test "SSD Sequential 64K" ssd_on /home/nsd/nsd_test_ssd.dat read 64k 0 15
    ;;
hdd)
    echo hdd > "$NSD_SYSFS/dev_class"
    run_test "HDD Sequential 64K" hdd_seq /mnt/hdd/nsd_hdd_test.dat read 64k 0 15
    run_test "HDD Random 4K" hdd_rand /mnt/hdd/nsd_hdd_test.dat randread 4k 0 20
    ;;
hdd_deep)
    echo hdd > "$NSD_SYSFS/dev_class"
    run_test "HDD Seq 64K (direct=0)" hdd_seq /mnt/hdd/nsd_hdd_test.dat read 64k 0 15
    run_test "HDD Seq 64K (direct=1)" hdd_seq_d1 /mnt/hdd/nsd_hdd_test.dat read 64k 1 15
    run_test "HDD Rand 4K (direct=0)" hdd_rand /mnt/hdd/nsd_hdd_test.dat randread 4k 0 20
    run_test "HDD Rand 4K (direct=1)" hdd_rand_d1 /mnt/hdd/nsd_hdd_test.dat randread 4k 1 20
    run_test "HDD Mixed 80/20 (direct=1)" hdd_mix /mnt/hdd/nsd_hdd_test.dat "read:80%randread:20%" 64k 1 20
    ;;
all)
    $0 ssd; $0 hdd_deep
    ;;
esac

echo "=== Done ==="
cat "$NSD_SYSFS/stats" 2>/dev/null | grep -E "hit_rate:|prefetch_amp:|rec_acc"
