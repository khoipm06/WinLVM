#pragma once

#include <string>
#include <vector>

namespace lvmfmt {

// PV subcommands
int cmd_pvcreate(const std::vector<std::string>& args);
int cmd_pvremove(const std::vector<std::string>& args);
int cmd_pvs(const std::vector<std::string>& args);
int cmd_pvdisplay(const std::vector<std::string>& args);
int cmd_pvscan(const std::vector<std::string>& args);
int cmd_pvresize(const std::vector<std::string>& args);
int cmd_pvchange(const std::vector<std::string>& args);
int cmd_pvmove(const std::vector<std::string>& args);
int cmd_pvck(const std::vector<std::string>& args);

// VG subcommands
int cmd_vgcreate(const std::vector<std::string>& args);
int cmd_vgremove(const std::vector<std::string>& args);
int cmd_vgs(const std::vector<std::string>& args);
int cmd_vgdisplay(const std::vector<std::string>& args);
int cmd_vgscan(const std::vector<std::string>& args);
int cmd_vgchange(const std::vector<std::string>& args);
int cmd_vgextend(const std::vector<std::string>& args);
int cmd_vgreduce(const std::vector<std::string>& args);
int cmd_vgrename(const std::vector<std::string>& args);
int cmd_vgcfgbackup(const std::vector<std::string>& args);
int cmd_vgcfgrestore(const std::vector<std::string>& args);
int cmd_vgck(const std::vector<std::string>& args);
int cmd_vgexport(const std::vector<std::string>& args);
int cmd_vgimport(const std::vector<std::string>& args);

// LV subcommands
int cmd_lvcreate(const std::vector<std::string>& args);
int cmd_lvremove(const std::vector<std::string>& args);
int cmd_lvs(const std::vector<std::string>& args);
int cmd_lvdisplay(const std::vector<std::string>& args);
int cmd_lvscan(const std::vector<std::string>& args);
int cmd_lvchange(const std::vector<std::string>& args);
int cmd_lvextend(const std::vector<std::string>& args);
int cmd_lvreduce(const std::vector<std::string>& args);
int cmd_lvresize(const std::vector<std::string>& args);
int cmd_lvrename(const std::vector<std::string>& args);
int cmd_lvconvert(const std::vector<std::string>& args);

// Misc subcommands
int cmd_dumpconfig(const std::vector<std::string>& args);
int cmd_lvmconfig(const std::vector<std::string>& args);
int cmd_fullreport(const std::vector<std::string>& args);
int cmd_formats(const std::vector<std::string>& args);
int cmd_segtypes(const std::vector<std::string>& args);
int cmd_tags(const std::vector<std::string>& args);
int cmd_systemid(const std::vector<std::string>& args);
int cmd_lvmdiskscan(const std::vector<std::string>& args);
int cmd_lvmdump(const std::vector<std::string>& args);
int cmd_help(const std::vector<std::string>& args);
int cmd_version(const std::vector<std::string>& args);

} // namespace lvmfmt
