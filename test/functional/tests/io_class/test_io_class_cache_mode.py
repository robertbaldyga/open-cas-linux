#
# Copyright(c) 2026 Unvertical
# SPDX-License-Identifier: BSD-3-Clause
#

import pytest

from api.cas import casadm
from api.cas.cache_config import (
    CacheMode,
    CacheModeTrait,
    CleaningPolicy,
    SeqCutOffPolicy,
)
from api.cas.ioclass_config import IoClass
from core.test_run import TestRun
from storage_devices.disk import DiskType, DiskTypeSet, DiskTypeLowerThan
from test_tools.dd import Dd
from test_tools.udev import Udev
from type_def.size import Size, Unit

ioclass_config_path = "/tmp/opencas_ioclass.conf"
core_size = Size(2, Unit.GibiByte)
io_size = Size(256, Unit.MebiByte)


@pytest.mark.parametrize(
    "cache_mode, io_class_cache_mode",
    [(CacheMode.WT, CacheMode.WB), (CacheMode.WB, CacheMode.WT)],
)
@pytest.mark.require_disk("cache", DiskTypeSet([DiskType.optane, DiskType.nand]))
@pytest.mark.require_disk("core", DiskTypeLowerThan("cache"))
def test_io_class_cache_mode(cache_mode, io_class_cache_mode):
    """
    title: Cache mode of an IO class
    description: |
        Set a cache mode for one IO class and check that writes classified to it follow that
        mode, while writes to an IO class with the "default" cache mode follow the mode of the
        cache.
    pass_criteria:
      - Listed IO class configuration matches the loaded one, cache modes included
      - Writes leave dirty data only in IO classes whose effective cache mode is lazy-write
      - The cache mode of an IO class is kept after stopping and loading the cache
      - Loading the IO class again with the "default" cache mode clears its cache mode
    """
    half_lba = int(core_size.get_value(Unit.Blocks512)) // 2
    with_cache_mode = IoClass(1, f"lba:lt:{half_lba}&done", 22, "1.00", io_class_cache_mode)
    default_cache_mode = IoClass(2, f"lba:ge:{half_lba}&done", 22, "1.00")
    io_classes = IoClass.default_list() + [with_cache_mode, default_cache_mode]

    with TestRun.step("Prepare cache and core devices"):
        cache_device = TestRun.disks["cache"]
        core_device = TestRun.disks["core"]
        cache_device.create_partitions([Size(2, Unit.GibiByte)])
        core_device.create_partitions([core_size])
        cache_device = cache_device.partitions[0]
        core_device = core_device.partitions[0]
        Udev.disable()

    with TestRun.step(f"Start cache in {cache_mode.name}, set NOP cleaning, add core"):
        cache = casadm.start_cache(cache_device, cache_mode=cache_mode, force=True)
        casadm.set_param_cleaning(cache_id=cache.cache_id, policy=CleaningPolicy.nop)
        core = casadm.add_core(cache, core_dev=core_device)
        core.set_seq_cutoff_policy(SeqCutOffPolicy.never)

    with TestRun.step(
        f"Load IO classes: lower half of the core in {io_class_cache_mode.name}, "
        "upper half with the default cache mode"
    ):
        IoClass.save_list_to_config_file(
            io_classes, add_default_rule=False, ioclass_config_path=ioclass_config_path
        )
        cache.load_io_class(ioclass_config_path)

    with TestRun.step("Check that the listed configuration matches the loaded one"):
        listed = cache.list_io_classes()
        if not IoClass.compare_ioclass_lists(listed, io_classes):
            TestRun.LOGGER.error(
                "Listed IO class configuration does not match the loaded one\n"
                f"Listed:\n{IoClass.list_to_csv(listed, False)}\n"
                f"Loaded:\n{IoClass.list_to_csv(io_classes, False)}"
            )

    with TestRun.step("Write to both halves of the core"):
        cache.purge_cache()
        cache.reset_counters()
        for seek in [0, int(core_size.get_value(Unit.MebiByte)) // 2]:
            (
                Dd()
                .input("/dev/zero")
                .output(core.path)
                .block_size(Size(1, Unit.MebiByte))
                .count(int(io_size.get_value(Unit.MebiByte)))
                .seek(seek)
                .oflag("direct")
                .run()
            )

    with TestRun.step("Check dirty data of both IO classes"):
        for io_class, effective_mode in [
            (with_cache_mode, io_class_cache_mode),
            (default_cache_mode, cache_mode),
        ]:
            usage = core.get_io_class_statistics(io_class_id=io_class.id).usage_stats
            lazy = bool(CacheMode.get_traits(effective_mode) & CacheModeTrait.LazyWrites)
            expected_dirty = io_size if lazy else Size.zero()
            if usage.occupancy != io_size:
                TestRun.LOGGER.error(
                    f"IO class {io_class.id} ({effective_mode.name}): occupancy "
                    f"{usage.occupancy}, expected {io_size}"
                )
            if usage.dirty != expected_dirty:
                TestRun.LOGGER.error(
                    f"IO class {io_class.id} ({effective_mode.name}): dirty "
                    f"{usage.dirty}, expected {expected_dirty}"
                )

    with TestRun.step("Stop and load the cache, check that the cache modes are kept"):
        cache.stop()
        cache = casadm.load_cache(cache_device)
        listed = cache.list_io_classes()
        if not IoClass.compare_ioclass_lists(listed, io_classes):
            TestRun.LOGGER.error(
                "IO class configuration changed after loading the cache\n"
                f"Listed:\n{IoClass.list_to_csv(listed, False)}\n"
                f"Expected:\n{IoClass.list_to_csv(io_classes, False)}"
            )

    with TestRun.step('Load the IO class again with the "default" cache mode'):
        all_default = IoClass.default_list() + [
            IoClass(1, with_cache_mode.rule, 22, "1.00"),
            default_cache_mode,
        ]
        IoClass.save_list_to_config_file(
            all_default, add_default_rule=False, ioclass_config_path=ioclass_config_path
        )
        cache.load_io_class(ioclass_config_path)
        listed = cache.list_io_classes()
        if not IoClass.compare_ioclass_lists(listed, all_default):
            TestRun.LOGGER.error(
                'IO class cache mode not cleared by "default"\n'
                f"Listed:\n{IoClass.list_to_csv(listed, False)}\n"
                f"Expected:\n{IoClass.list_to_csv(all_default, False)}"
            )
