#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace lvmfmt {

struct CommandDesc {
    std::string name;
    std::string summary;
    std::string syntax;
    std::string description;
    std::vector<std::string> options;
};

inline const std::vector<CommandDesc>& get_all_commands() {
    static const std::vector<CommandDesc> commands = {
        // PV Commands
        {
            "pvcreate",
            "Initialize physical volume(s) for use by LVM",
            "pvcreate [OPTIONS] DevicePath [DevicePath...]",
            "pvcreate initializes a Physical Volume on a block device or raw disk image so that it can be used by LVM2 Volume Groups. It writes an LVM2 label (LABELONE) and prepares the metadata area.",
            {
                "-f, --force : Force creation without interactive confirmation",
                "-y, --yes : Answer yes to all questions",
                "-u, --uuid UUID : Specify a custom UUID for the physical volume",
                "--setphysicalvolumesize Size : Override the detected device size",
                "--metadatasize Size : Size of the metadata area (default 1 MiB)",
                "--dataalignment Size : Alignment of the data area / PE start (default 1 MiB)"
            }
        },
        {
            "pvremove",
            "Remove LVM label(s) from physical volume(s)",
            "pvremove [OPTIONS] DevicePath [DevicePath...]",
            "pvremove wipes the LVM2 label and metadata area from the specified physical volume, making it no longer recognized as an LVM PV.",
            {
                "-f, --force : Force wipe even if PV belongs to a Volume Group",
                "-y, --yes : Answer yes to all questions"
            }
        },
        {
            "pvs",
            "Display information about physical volumes",
            "pvs [OPTIONS] [DevicePath...]",
            "pvs produces formatted summary output about physical volumes, their associated volume groups, sizes, free extents, and attributes.",
            {
                "-a, --all : Display all devices, even uninitialized ones",
                "-o, --options Columns : Comma-separated list of columns to display",
                "--noheadings : Suppress heading lines",
                "--separator Char : Separator between columns"
            }
        },
        {
            "pvdisplay",
            "Display detailed attributes of physical volume(s)",
            "pvdisplay [OPTIONS] [DevicePath...]",
            "pvdisplay shows detailed multi-line output for physical volumes including UUID, PE size, total/allocated/free extents.",
            {
                "-c, --colon : Output in colon-separated format",
                "-s, --short : Display only device name and size",
                "-m, --maps : Display physical extent to logical extent mappings"
            }
        },
        {
            "pvscan",
            "Scan all supported disks for physical volumes",
            "pvscan [OPTIONS]",
            "pvscan scans system devices, partitions, and candidate storage devices to discover all physical volumes and populate internal caches.",
            {
                "-e, --exported : Only report exported volume groups",
                "-v, --verbose : Detailed discovery logging"
            }
        },
        {
            "pvresize",
            "Resize physical volume(s)",
            "pvresize [OPTIONS] DevicePath",
            "pvresize updates the size of a physical volume after the underlying disk or partition has been enlarged or shrunk.",
            {
                "--setphysicalvolumesize Size : Manually specify new physical volume size"
            }
        },
        {
            "pvchange",
            "Change attributes of physical volume(s)",
            "pvchange [OPTIONS] DevicePath",
            "pvchange alters physical volume allocation policies (e.g. enabling or disabling extent allocation).",
            {
                "-x, --allocatable {y|n} : Enable or disable allocation of new extents on this PV",
                "-u, --uuid : Generate and assign a new random UUID to the PV"
            }
        },
        {
            "pvmove",
            "Move extents from one physical volume to another",
            "pvmove [OPTIONS] SourcePV [DestinationPV...]",
            "pvmove copies allocated physical extents from SourcePV to DestinationPV, updating the volume group metadata and freeing space on SourcePV.",
            {
                "-n, --name LogicalVolume : Only move extents belonging to the specified LV",
                "-b, --background : Run migration in background daemon mode",
                "-i, --interval Sec : Polling interval for progress reporting",
                "-v, --verbose : Detailed extent-by-extent migration progress"
            }
        },
        {
            "pvck",
            "Check metadata on physical volumes",
            "pvck [OPTIONS] DevicePath",
            "pvck verifies the integrity of the LVM label, checksums, and circular MDA metadata area on a physical volume.",
            {
                "-v, --verbose : Detailed verification report"
            }
        },

        // VG Commands
        {
            "vgcreate",
            "Create a volume group",
            "vgcreate [OPTIONS] VGName DevicePath [DevicePath...]",
            "vgcreate creates a new Volume Group containing one or more physical volumes. Initializes extent mapping and writes initial metadata text.",
            {
                "-s, --physicalextentsize Size : Physical extent size (default 4 MiB)",
                "-f, --force : Force creation",
                "-y, --yes : Answer yes to all questions"
            }
        },
        {
            "vgremove",
            "Remove volume group(s)",
            "vgremove [OPTIONS] VGName",
            "vgremove deletes a volume group and frees its physical volumes.",
            {
                "-f, --force : Remove even if logical volumes are active",
                "-y, --yes : Answer yes to all questions"
            }
        },
        {
            "vgs",
            "Display information about volume groups",
            "vgs [OPTIONS] [VGName...]",
            "vgs produces formatted summary output about volume groups, PV count, LV count, total size, and free space.",
            {
                "-o, --options Columns : Comma-separated list of columns to display",
                "--noheadings : Suppress heading lines",
                "--separator Char : Separator between columns"
            }
        },
        {
            "vgdisplay",
            "Display detailed attributes of volume group(s)",
            "vgdisplay [OPTIONS] [VGName...]",
            "vgdisplay shows detailed multi-line output for volume groups including UUID, extent size, total/allocated/free extents.",
            {
                "-c, --colon : Output in colon-separated format",
                "-s, --short : Display only summary"
            }
        },
        {
            "vgscan",
            "Search for all volume groups",
            "vgscan [OPTIONS]",
            "vgscan searches all physical volumes for volume group metadata and triggers driver device re-probe.",
            {
                "-v, --verbose : Verbose scanning output"
            }
        },
        {
            "vgchange",
            "Change volume group attributes",
            "vgchange [OPTIONS] [VGName] [PVPath...]",
            "vgchange modifies volume group state, such as activating (-ay) or deactivating (-an) logical volumes or changing read/write permissions (-p r|rw).",
            {
                "-a, --activate {y|n|ay|an} : Activate or deactivate logical volumes in the VG",
                "-p, --permission {r|rw} : Set read-only (r) or read-write (rw) permission"
            }
        },
        {
            "vgextend",
            "Add physical volumes to a volume group",
            "vgextend [OPTIONS] VGName DevicePath [DevicePath...]",
            "vgextend expands a volume group by adding one or more initialized physical volumes.",
            {
                "-f, --force : Force addition",
                "-y, --yes : Answer yes to all questions"
            }
        },
        {
            "vgreduce",
            "Remove physical volume(s) from a volume group",
            "vgreduce [OPTIONS] VGName DevicePath [DevicePath...]",
            "vgreduce shrinks a volume group by removing unused physical volumes.",
            {
                "-a, --all : Remove all empty physical volumes from the VG"
            }
        },
        {
            "vgrename",
            "Rename a volume group",
            "vgrename [OPTIONS] OldVGName NewVGName",
            "vgrename changes the name of a volume group and updates all metadata records.",
            {}
        },
        {
            "vgcfgbackup",
            "Backup volume group configuration(s)",
            "vgcfgbackup [OPTIONS] [VGName...]",
            "vgcfgbackup saves the text metadata representation of one or all volume groups to a backup file.",
            {
                "-f, --file BackupPath : Write backup to specified file path"
            }
        },
        {
            "vgcfgrestore",
            "Restore volume group configuration",
            "vgcfgrestore -f BackupPath VGName",
            "vgcfgrestore reads metadata text from an archive file and restores it to the physical volumes of the volume group.",
            {
                "-f, --file BackupPath : File containing VG metadata backup"
            }
        },
        {
            "vgck",
            "Check the consistency of volume group(s)",
            "vgck [OPTIONS] [VGName...]",
            "vgck checks the consistency and metadata CRC of the volume group across all member physical volumes.",
            {}
        },
        {
            "vgexport",
            "Export volume group(s) to make them inactive for relocation",
            "vgexport [OPTIONS] VGName",
            "vgexport marks a volume group as exported so it cannot be modified until imported.",
            {}
        },
        {
            "vgimport",
            "Import exported volume group(s)",
            "vgimport [OPTIONS] VGName",
            "vgimport registers an exported volume group for active use on this host.",
            {}
        },

        // LV Commands
        {
            "lvcreate",
            "Create a logical volume",
            "lvcreate [OPTIONS] -L Size -n LVName VGName",
            "lvcreate allocates physical extents from the specified volume group and creates a new logical volume (linear or striped).",
            {
                "-n, --name Name : Name of the new logical volume",
                "-L, --size Size : Size of the logical volume (e.g. 100M, 2G)",
                "-l, --extents Extents : Number of extents to allocate",
                "-i, --stripes Count : Number of stripes for striped LV",
                "-I, --stripesize Size : Stripe chunk size (default 64 KiB)",
                "-y, --yes : Answer yes to all questions"
            }
        },
        {
            "lvremove",
            "Remove logical volume(s) from the system",
            "lvremove [OPTIONS] [VGName/]LVName",
            "lvremove deletes a logical volume, frees its allocated physical extents, and unbinds its volume device.",
            {
                "-f, --force : Force removal",
                "-y, --yes : Answer yes to all questions"
            }
        },
        {
            "lvs",
            "Display information about logical volumes",
            "lvs [OPTIONS] [[VGName/]LVName...]",
            "lvs produces formatted summary output about logical volumes, layout, size, and mapped segments.",
            {
                "-o, --options Columns : Comma-separated list of columns to display",
                "--noheadings : Suppress heading lines",
                "--separator Char : Separator between columns"
            }
        },
        {
            "lvdisplay",
            "Display detailed attributes of a logical volume",
            "lvdisplay [OPTIONS] [[VGName/]LVName...]",
            "lvdisplay shows detailed multi-line output for logical volumes including UUID, status, size, segments, and stripe layout.",
            {
                "-c, --colon : Output in colon-separated format",
                "-m, --maps : Display logical extent to physical extent mappings"
            }
        },
        {
            "lvscan",
            "List all logical volumes in all volume groups",
            "lvscan [OPTIONS]",
            "lvscan scans for all logical volumes and lists their active status and capacities.",
            {}
        },
        {
            "lvchange",
            "Change the attributes of logical volume(s)",
            "lvchange [OPTIONS] [VGName/]LVName",
            "lvchange modifies logical volume runtime state (e.g. active/inactive, read-only/read-write).",
            {
                "-a, --activate {y|n|ay|an} : Activate or deactivate the logical volume",
                "-p, --permission {r|rw} : Set read-only (r) or read-write (rw) permission"
            }
        },
        {
            "lvextend",
            "Add space to a logical volume",
            "lvextend [OPTIONS] -L +Size [VGName/]LVName",
            "lvextend expands the capacity of a logical volume by allocating additional physical extents from the volume group.",
            {
                "-L, --size [+]Size : Amount of space to add (or new total size)",
                "-l, --extents [+]Extents : Number of extents to add",
                "-r, --resizefs : Request filesystem resize after expansion"
            }
        },
        {
            "lvreduce",
            "Reduce the size of a logical volume",
            "lvreduce [OPTIONS] -L -Size [VGName/]LVName",
            "lvreduce reduces the capacity of a logical volume and frees the trailing physical extents.",
            {
                "-L, --size [-]Size : Amount of space to remove (or new total size)",
                "-l, --extents [-]Extents : Number of extents to reduce",
                "-f, --force : Force reduction without safety prompt",
                "-y, --yes : Answer yes to all questions"
            }
        },
        {
            "lvresize",
            "Resize a logical volume",
            "lvresize [OPTIONS] -L Size [VGName/]LVName",
            "lvresize resizes a logical volume to the specified target capacity, expanding or reducing extents as necessary.",
            {
                "-L, --size [+/-]Size : New size (absolute or relative)",
                "-l, --extents [+/-]Extents : New extent count",
                "-f, --force : Force reduction if shrinking",
                "-y, --yes : Answer yes to all questions"
            }
        },
        {
            "lvrename",
            "Rename a logical volume",
            "lvrename [OPTIONS] VGName OldLVName NewLVName",
            "lvrename changes the name of an existing logical volume in the volume group.",
            {}
        },
        {
            "lvconvert",
            "Change logical volume layout or segment type",
            "lvconvert [OPTIONS] [VGName/]LVName",
            "lvconvert changes the segment layout of a logical volume (e.g. converting between linear and striped).",
            {}
        },

        // Reporting, Config & System Commands
        {
            "version",
            "Display software and driver version information",
            "version",
            "version prints the version of the WinLVM subsystem, user-space CLI, and kernel driver status.",
            {}
        },
        {
            "help",
            "Display help for commands",
            "help [Command]",
            "help displays usage syntax, available flags, and descriptions for LVM commands.",
            {}
        },
        {
            "dumpconfig",
            "Display and manipulate configuration information",
            "dumpconfig [OPTIONS]",
            "dumpconfig prints configuration settings and defaults in LVM2 config format.",
            {}
        },
        {
            "formats",
            "List available metadata formats",
            "formats",
            "formats lists the supported LVM metadata formats (e.g. lvm2).",
            {}
        },
        {
            "segtypes",
            "List available segment types",
            "segtypes",
            "segtypes lists the segment types supported by this implementation (linear, striped).",
            {}
        },
        {
            "tags",
            "List tags defined on this host",
            "tags",
            "tags lists tags associated with volume groups and logical volumes.",
            {}
        },
        {
            "systemid",
            "Display the system ID, if any, currently set on this host",
            "systemid",
            "systemid displays the host system ID used for LVM2 cluster locking and ownership.",
            {}
        },
        {
            "lvmdiskscan",
            "List devices that may be used as physical volumes",
            "lvmdiskscan [OPTIONS]",
            "lvmdiskscan scans the system for candidate disks, partitions, and virtual devices.",
            {}
        },
        {
            "lvmdump",
            "Create a dump of LVM metadata, driver state, and diagnostics",
            "lvmdump [OPTIONS]",
            "lvmdump gathers diagnostic information, raw metadata dumps, and kernel telemetry.",
            {
                "-d, --destination Dir : Directory to save diagnostic dump archive"
            }
        }
    };
    return commands;
}

inline const CommandDesc* find_command(std::string_view name) {
    const auto& cmds = get_all_commands();
    for (const auto& cmd : cmds) {
        if (cmd.name == name) return &cmd;
    }
    return nullptr;
}

} // namespace lvmfmt
