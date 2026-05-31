#include "winlvm/json.h"

#include <algorithm>
#include <sstream>
#include <tuple>

namespace winlvm {

namespace {

std::string escape_json(const std::string& value) {
  std::ostringstream out;
  for (const char c : value) {
    switch (c) {
      case '"':
        out << "\\\"";
        break;
      case '\\':
        out << "\\\\";
        break;
      case '\n':
        out << "\\n";
        break;
      case '\r':
        out << "\\r";
        break;
      case '\t':
        out << "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          out << "\\u00";
          const char* hex = "0123456789abcdef";
          out << hex[(static_cast<unsigned char>(c) >> 4U) & 0x0fU];
          out << hex[static_cast<unsigned char>(c) & 0x0fU];
        } else {
          out << c;
        }
    }
  }
  return out.str();
}

void indent(std::ostringstream& out, int level) {
  for (int i = 0; i < level; ++i) {
    out << "  ";
  }
}

void string_field(std::ostringstream& out, int level, const char* name, const std::string& value, bool comma = true) {
  indent(out, level);
  out << '"' << name << "\": \"" << escape_json(value) << '"';
  if (comma) {
    out << ',';
  }
  out << '\n';
}

void number_field(std::ostringstream& out, int level, const char* name, std::uint64_t value, bool comma = true) {
  indent(out, level);
  out << '"' << name << "\": " << value;
  if (comma) {
    out << ',';
  }
  out << '\n';
}

void bool_field(std::ostringstream& out, int level, const char* name, bool value, bool comma = true) {
  indent(out, level);
  out << '"' << name << "\": " << (value ? "true" : "false");
  if (comma) {
    out << ',';
  }
  out << '\n';
}

void string_array(std::ostringstream& out, const std::vector<std::string>& items) {
  out << '[';
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i != 0) {
      out << ", ";
    }
    out << '"' << escape_json(items[i]) << '"';
  }
  out << ']';
}

}  // namespace

std::string to_json(const ScanResult& result) {
  std::ostringstream out;
  out << "{\n";
  number_field(out, 1, "schema_version", kJsonSchemaVersion);

  indent(out, 1);
  out << "\"inputs\": [\n";
  for (std::size_t i = 0; i < result.inputs.size(); ++i) {
    const InputImage& input = result.inputs[i];
    indent(out, 2);
    out << "{\n";
    string_field(out, 3, "source_type", input.source_type);
    string_field(out, 3, "path", input.path);
    number_field(out, 3, "size_bytes", input.size_bytes);
    indent(out, 3);
    out << "\"candidate_pv_base_offsets\": [";
    for (std::size_t j = 0; j < input.candidate_pv_base_offsets.size(); ++j) {
      if (j != 0) {
        out << ", ";
      }
      out << input.candidate_pv_base_offsets[j];
    }
    out << "]\n";
    indent(out, 2);
    out << "}";
    if (i + 1 != result.inputs.size()) {
      out << ',';
    }
    out << '\n';
  }
  indent(out, 1);
  out << "],\n";

  indent(out, 1);
  out << "\"pvs\": [\n";
  std::vector<PhysicalVolume> pvs = result.pvs;
  std::sort(pvs.begin(), pvs.end(), [](const PhysicalVolume& a, const PhysicalVolume& b) {
    return a.uuid < b.uuid;
  });
  for (std::size_t i = 0; i < pvs.size(); ++i) {
    const PhysicalVolume& pv = pvs[i];
    indent(out, 2);
    out << "{\n";
    string_field(out, 3, "name", pv.name);
    string_field(out, 3, "uuid", pv.uuid);
    string_field(out, 3, "source_path", pv.source_path);
    number_field(out, 3, "pv_base_offset_bytes", pv.pv_base_offset_bytes);
    number_field(out, 3, "label_offset_bytes", pv.label_offset_bytes);
    number_field(out, 3, "device_size_bytes", pv.device_size_bytes);
    number_field(out, 3, "pe_start_bytes", pv.pe_start_bytes);
    number_field(out, 3, "pe_count", pv.pe_count);
    indent(out, 3);
    out << "\"metadata_area_offsets\": [";
    for (std::size_t j = 0; j < pv.metadata_area_offsets.size(); ++j) {
      if (j != 0) {
        out << ", ";
      }
      out << pv.metadata_area_offsets[j];
    }
    out << "]\n";
    indent(out, 2);
    out << "}";
    if (i + 1 != pvs.size()) {
      out << ',';
    }
    out << '\n';
  }
  indent(out, 1);
  out << "],\n";

  indent(out, 1);
  out << "\"metadata_copies\": [\n";
  for (std::size_t i = 0; i < result.metadata_copies.size(); ++i) {
    const MetadataCopy& copy = result.metadata_copies[i];
    indent(out, 2);
    out << "{\n";
    string_field(out, 3, "pv_uuid", copy.pv_uuid);
    string_field(out, 3, "source_path", copy.source_path);
    number_field(out, 3, "pv_base_offset_bytes", copy.pv_base_offset_bytes);
    number_field(out, 3, "mda_offset_bytes", copy.mda_offset_bytes);
    number_field(out, 3, "mda_size_bytes", copy.mda_size_bytes);
    bool_field(out, 3, "header_checksum_valid", copy.header_checksum_valid);
    bool_field(out, 3, "magic_valid", copy.magic_valid);
    bool_field(out, 3, "selected", copy.selected);
    string_field(out, 3, "rejected_reason", copy.rejected_reason);
    string_field(out, 3, "vg_name", copy.vg_name);
    string_field(out, 3, "vg_uuid", copy.vg_uuid);
    number_field(out, 3, "seqno", copy.seqno);
    indent(out, 3);
    out << "\"raw_locations\": [\n";
    for (std::size_t j = 0; j < copy.raw_locations.size(); ++j) {
      const MetadataRawLocation& raw = copy.raw_locations[j];
      indent(out, 4);
      out << "{\n";
      number_field(out, 5, "offset_bytes", raw.offset_bytes);
      number_field(out, 5, "size_bytes", raw.size_bytes);
      number_field(out, 5, "checksum", raw.checksum);
      number_field(out, 5, "actual_checksum", raw.actual_checksum);
      number_field(out, 5, "flags", raw.flags);
      bool_field(out, 5, "checksum_valid", raw.checksum_valid);
      bool_field(out, 5, "ignored", raw.ignored, false);
      indent(out, 4);
      out << "}";
      if (j + 1 != copy.raw_locations.size()) {
        out << ',';
      }
      out << '\n';
    }
    indent(out, 3);
    out << "]\n";
    indent(out, 2);
    out << "}";
    if (i + 1 != result.metadata_copies.size()) {
      out << ',';
    }
    out << '\n';
  }
  indent(out, 1);
  out << "],\n";

  indent(out, 1);
  out << "\"selected_metadata\": ";
  const auto selected = std::find_if(result.metadata_copies.begin(), result.metadata_copies.end(),
                                    [](const MetadataCopy& copy) { return copy.selected; });
  if (selected == result.metadata_copies.end()) {
    out << "null,\n";
  } else {
    out << "{ \"vg_name\": \"" << escape_json(selected->vg_name) << "\", \"vg_uuid\": \""
        << escape_json(selected->vg_uuid) << "\", \"seqno\": " << selected->seqno << " },\n";
  }

  indent(out, 1);
  out << "\"vgs\": [\n";
  for (std::size_t i = 0; i < result.vgs.size(); ++i) {
    const VolumeGroup& vg = result.vgs[i];
    indent(out, 2);
    out << "{\n";
    string_field(out, 3, "name", vg.name);
    string_field(out, 3, "uuid", vg.uuid);
    number_field(out, 3, "seqno", vg.seqno);
    number_field(out, 3, "extent_size_sectors", vg.extent_size_sectors);
    number_field(out, 3, "extent_size_bytes", vg.extent_size_bytes);
    indent(out, 3);
    out << "\"status\": ";
    string_array(out, vg.status);
    out << '\n';
    indent(out, 2);
    out << "}";
    if (i + 1 != result.vgs.size()) {
      out << ',';
    }
    out << '\n';
  }
  indent(out, 1);
  out << "],\n";

  indent(out, 1);
  out << "\"lvs\": [\n";
  std::vector<LogicalVolume> lvs;
  for (const VolumeGroup& vg : result.vgs) {
    lvs.insert(lvs.end(), vg.lvs.begin(), vg.lvs.end());
  }
  std::sort(lvs.begin(), lvs.end(), [](const LogicalVolume& a, const LogicalVolume& b) {
    return std::tie(a.vg_name, a.name, a.uuid) < std::tie(b.vg_name, b.name, b.uuid);
  });
  for (std::size_t i = 0; i < lvs.size(); ++i) {
    const LogicalVolume& lv = lvs[i];
    indent(out, 2);
    out << "{\n";
    string_field(out, 3, "name", lv.name);
    string_field(out, 3, "vg_name", lv.vg_name);
    string_field(out, 3, "uuid", lv.uuid);
    number_field(out, 3, "size_bytes", lv.size_bytes);
    bool_field(out, 3, "visible", lv.visible);
    bool_field(out, 3, "supported", lv.supported);
    string_field(out, 3, "unsupported_reason", lv.unsupported_reason);
    indent(out, 3);
    out << "\"status\": ";
    string_array(out, lv.status);
    out << ",\n";
    indent(out, 3);
    out << "\"segments\": [\n";
    for (std::size_t j = 0; j < lv.segments.size(); ++j) {
      const Segment& segment = lv.segments[j];
      indent(out, 4);
      out << "{\n";
      number_field(out, 5, "start_extent", segment.start_extent);
      number_field(out, 5, "extent_count", segment.extent_count);
      string_field(out, 5, "type", segment.type);
      string_field(out, 5, "pv_name", segment.pv_name);
      string_field(out, 5, "pv_uuid", segment.pv_uuid);
      number_field(out, 5, "pv_start_extent", segment.pv_start_extent);
      number_field(out, 5, "lv_start_bytes", segment.lv_start_bytes);
      number_field(out, 5, "length_bytes", segment.length_bytes);
      number_field(out, 5, "pv_start_bytes", segment.pv_start_bytes, false);
      indent(out, 4);
      out << "}";
      if (j + 1 != lv.segments.size()) {
        out << ',';
      }
      out << '\n';
    }
    indent(out, 3);
    out << "]\n";
    indent(out, 2);
    out << "}";
    if (i + 1 != lvs.size()) {
      out << ',';
    }
    out << '\n';
  }
  indent(out, 1);
  out << "],\n";

  indent(out, 1);
  out << "\"mappings\": [\n";
  std::vector<MappingRange> mappings = result.mappings;
  std::sort(mappings.begin(), mappings.end(), [](const MappingRange& a, const MappingRange& b) {
    return std::tie(a.vg_name, a.lv_name, a.lv_start_bytes) < std::tie(b.vg_name, b.lv_name, b.lv_start_bytes);
  });
  for (std::size_t i = 0; i < mappings.size(); ++i) {
    const MappingRange& mapping = mappings[i];
    indent(out, 2);
    out << "{\n";
    string_field(out, 3, "vg_name", mapping.vg_name);
    string_field(out, 3, "lv_name", mapping.lv_name);
    string_field(out, 3, "lv_uuid", mapping.lv_uuid);
    number_field(out, 3, "lv_start_bytes", mapping.lv_start_bytes);
    number_field(out, 3, "length_bytes", mapping.length_bytes);
    string_field(out, 3, "pv_uuid", mapping.pv_uuid);
    number_field(out, 3, "pv_start_bytes", mapping.pv_start_bytes);
    string_field(out, 3, "source_path", mapping.source_path);
    number_field(out, 3, "source_offset_bytes", mapping.source_offset_bytes, false);
    indent(out, 2);
    out << "}";
    if (i + 1 != mappings.size()) {
      out << ',';
    }
    out << '\n';
  }
  indent(out, 1);
  out << "],\n";

  indent(out, 1);
  out << "\"diagnostics\": [\n";
  for (std::size_t i = 0; i < result.diagnostics.size(); ++i) {
    const Diagnostic& diagnostic = result.diagnostics[i];
    indent(out, 2);
    out << "{\n";
    string_field(out, 3, "severity", diagnostic.severity);
    string_field(out, 3, "code", diagnostic.code);
    string_field(out, 3, "message", diagnostic.message, false);
    indent(out, 2);
    out << "}";
    if (i + 1 != result.diagnostics.size()) {
      out << ',';
    }
    out << '\n';
  }
  indent(out, 1);
  out << "],\n";

  indent(out, 1);
  out << "\"oracle\": { \"source\": \"linux-lvm2-dmsetup\", \"required_for_golden_tests\": true }\n";
  out << "}\n";
  return out.str();
}

}  // namespace winlvm
