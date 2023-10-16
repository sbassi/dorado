#include "SampleSheet.h"

#include "PostCondition.h"
#include "barcode_kits.h"
#include "string_utils.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <regex>

namespace {

const size_t MAX_USER_FIELD_LENGTH = 40;

// clang-format off
std::vector<std::string> allowed_column_names{
    // Standard
    "protocol_run_id",
    "flow_cell_id",
    "position_id",
    "sample_id",
    "experiment_id",
    "flow_cell_product_code",
    "kit",
    // barcoding
    "alias",
    "type",
    "barcode"
};
// clang-format on

std::vector<std::string> csv_to_tokens(const std::string& input) {
    return dorado::utils::split(input, ',');
}

bool is_valid_mk_freetext(const std::string& input) {
    if (input.size() > MAX_USER_FIELD_LENGTH) {
        return false;
    }

    return std::all_of(std::begin(input), std::end(input), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_';
    });
}

bool is_alias_forbidden(const std::string& input) {
    const auto& barcode_ids = dorado::barcode_kits::get_barcode_identifiers();
    return barcode_ids.count(input) != 0;
}

bool get_line(std::istream& input,
              dorado::utils::details::EolFileFormat eol_format,
              std::string& target) {
    // linux EOL:   "\n"
    // windows EOL: "\r\n"
    // osx EOL:     "\r"
    char delimiter = eol_format == dorado::utils::details::EolFileFormat::osx_eol ? '\r' : '\n';
    if (!std::getline(input, target, delimiter)) {
        return false;
    }

    if (eol_format == dorado::utils::details::EolFileFormat::windows_eol && !target.empty() &&
        *target.rbegin() == '\r') {
        target.pop_back();
    }
    return true;
}

}  // anonymous namespace

namespace dorado::utils {

SampleSheet::SampleSheet(const std::string& filename, bool skip_index_matching)
        : m_filename(filename), m_skip_index_matching(skip_index_matching) {
    if (!filename.empty()) {
        load(filename);
    }
}

void SampleSheet::load(const std::string& filename) {
    m_filename = filename;
    std::ifstream file_stream(filename);
    load(file_stream, filename);
}

void SampleSheet::load(std::istream& file_stream, const std::string& filename) {
    if (!file_stream) {
        throw std::runtime_error(std::string("Cannot open file ") + filename);
    }
    auto eol_format = details::get_eol_file_format(file_stream);
    // Fetch the column headers from the file
    std::string source_line;
    if (!get_line(file_stream, eol_format, source_line)) {
        throw std::runtime_error(std::string("Cannot read column headers from sample sheet file ") +
                                 filename);
    }
    auto col_names = csv_to_tokens(source_line);

    // Validate headers
    validate_headers(col_names, filename);

    // Create column header map
    for (size_t i = 0; i < col_names.size(); i++) {
        m_col_indices[col_names[i]] = i;
    }

    // Read in all the sample lines
    std::string expected_experiment_id;
    while (get_line(file_stream, eol_format, source_line)) {
        std::string experiment_id;
        auto row = csv_to_tokens(source_line);

        if (row.size() != m_col_indices.size()) {
            throw std::runtime_error(std::string("Row in sample sheet file " + filename +
                                                 " has incorrect number of entries"));
        }

        // All rows must have the same experiment ID
        experiment_id = row[m_col_indices["experiment_id"]];
        if (expected_experiment_id.empty()) {
            expected_experiment_id = experiment_id;
        }
        if (expected_experiment_id != experiment_id) {
            throw std::runtime_error(std::string("Sample sheet file " + filename +
                                                 " contains more than one experiment_id"));
        }

        // sample_id, experiment_id, and alias must be valid minknow free-text
        validate_text(row, "experiment_id");
        validate_text(row, "sample_id");
        validate_text(row, "alias");
        // alias cannot be a valid barcode id
        validate_alias(row, "alias");

        // Add the row
        m_rows.push_back(row);
    }

    if (m_skip_index_matching && !is_barcode_mapping_unique()) {
        throw std::runtime_error(
                std::string("Unable to infer barcode aliases from sample sheet file: " + filename +
                            " does not contain a unique mapping of barcode ids."));
    }
}

// check if we can generate a unique alias without the flowcell/position information
bool SampleSheet::is_barcode_mapping_unique() const {
    if (m_index[FLOW_CELL_ID]) {
        auto idx = m_col_indices.at("flow_cell_id");
        auto&& first_flow_cell_id = m_rows.at(0)[idx];
        for (const auto& row : m_rows) {
            if (row[idx] != first_flow_cell_id) {
                return false;
            }
        }
    }

    if (m_index[POSITION_ID]) {
        auto idx = m_col_indices.at("position_id");
        auto&& first_position_id = m_rows.at(0)[idx];
        for (const auto& row : m_rows) {
            if (row[idx] != first_position_id) {
                return false;
            }
        }
    }

    std::set<std::string> barcodes;
    auto idx = m_col_indices.at("barcode");
    for (const auto& row : m_rows) {
        barcodes.insert(row[idx]);
    }
    return barcodes.size() == m_rows.size();
}

std::string SampleSheet::get_alias(const std::string& flow_cell_id,
                                   const std::string& position_id,
                                   const std::string& experiment_id,
                                   const std::string& barcode) const {
    if (m_type != Type::barcode) {
        return "";
    }

    if (!check_index(flow_cell_id, position_id)) {
        return "";
    }

    for (const auto& row : m_rows) {
        if (match_index(row, flow_cell_id, position_id, experiment_id) &&
            get(row, "barcode") == barcode) {
            return get(row, "alias");
        }
    }

    // Didn't find an alias
    return "";
}

BarcodingInfo::FilterSet SampleSheet::get_barcode_values() const {
    std::unordered_set<std::string> barcodes;

    switch (m_type) {
    case Type::barcode: {
        // Grab the barcode idx once so that we're not doing it repeatedly
        const auto barcode_idx = m_col_indices.at("barcode");

        // Grab the barcodes
        for (const auto& row : m_rows) {
            barcodes.emplace(row[barcode_idx]);
        }
        break;
    }
    case Type::none:
        [[fallthrough]];
    default:
        return std::nullopt;
    }

    return barcodes;
}

void SampleSheet::validate_headers(const std::vector<std::string>& col_names,
                                   const std::string& filename) {
    m_type = Type::none;
    m_index.reset();

    // Each header must be in the allowed list
    for (auto& col_name : col_names) {
        if (std::find(allowed_column_names.begin(), allowed_column_names.end(), col_name) ==
            allowed_column_names.end()) {
            throw std::runtime_error(std::string("Sample sheet ") + filename +
                                     " contains invalid column " + col_name);
        }
    }

    if (std::find(col_names.begin(), col_names.end(), "flow_cell_id") != col_names.end()) {
        m_index.set(FLOW_CELL_ID);
    }
    if (std::find(col_names.begin(), col_names.end(), "position_id") != col_names.end()) {
        m_index.set(POSITION_ID);
    }

    // Either "flow_cell_id" or "position_id" must be specified.
    if (m_index.none()) {
        throw std::runtime_error(
                std::string("Sample sheet ") + filename +
                " must contain at least one of the 'flow_cell_id', and 'position_id' columns.");
    }

    // "experiment_id" column must be there
    if (std::find(col_names.begin(), col_names.end(), "experiment_id") == col_names.end()) {
        throw std::runtime_error(std::string("Sample sheet ") + filename +
                                 " must contain experiment_id column.");
    }

    // "kit" column must be there
    if (std::find(col_names.begin(), col_names.end(), "kit") == col_names.end()) {
        throw std::runtime_error(std::string("Sample sheet ") + filename +
                                 " must contain kit column.");
    }

    // Set up barcoding flag
    if (std::find(col_names.begin(), col_names.end(), "barcode") != col_names.end()) {
        m_type = Type::barcode;
    }

    // If any kind of barcoding is there, the alias must be there, and vice versa
    bool has_alias = std::find(col_names.begin(), col_names.end(), "alias") != col_names.end();
    if (m_type != Type::none && !has_alias) {
        throw std::runtime_error(std::string("Sample sheet ") + filename +
                                 " contains barcode columns but alias column is missing.");
    } else if (m_type == Type::none && has_alias) {
        throw std::runtime_error(std::string("Sample sheet ") + filename +
                                 " contains alias column but barcode columns are missing.");
    }
}

void SampleSheet::validate_text(const Row& row, const std::string& key) const {
    const auto col_idx = m_col_indices.find(key);
    if (col_idx != m_col_indices.end()) {
        if (!is_valid_mk_freetext(row[col_idx->second])) {
            throw std::runtime_error(key + " '" + row[col_idx->second] +
                                     "' is not a valid string (at most " +
                                     std::to_string(MAX_USER_FIELD_LENGTH) +
                                     " alphanumerical characters including '-' and '_')");
        }
    }
}

void SampleSheet::validate_alias(const Row& row, const std::string& key) const {
    const auto col_idx = m_col_indices.find(key);
    if (col_idx != m_col_indices.end()) {
        if (is_alias_forbidden(row[col_idx->second])) {
            throw std::runtime_error(key + " " + row[col_idx->second] + " is a forbidden alias");
        }
    }
}

bool SampleSheet::check_index(const std::string& flow_cell_id,
                              const std::string& position_id) const {
    return m_skip_index_matching || ((m_index[FLOW_CELL_ID] == !flow_cell_id.empty()) &&
                                     (m_index[POSITION_ID] == !position_id.empty()));
}

bool SampleSheet::match_index(const Row& row,
                              const std::string& flow_cell_id,
                              const std::string& position_id,
                              const std::string& experiment_id) const {
    return m_skip_index_matching ||
           ((!m_index[FLOW_CELL_ID] || get(row, "flow_cell_id") == flow_cell_id) &&
            (!m_index[POSITION_ID] || get(row, "position_id") == position_id) &&
            (get(row, "experiment_id") == experiment_id));
}

std::string SampleSheet::get(const Row& row, const std::string& key) const {
    const auto col_idx = m_col_indices.find(key);
    if (col_idx != m_col_indices.end()) {
        return row[col_idx->second];
    }
    return "";
}

std::string to_string(SampleSheet::Type type) {
    switch (type) {
    case SampleSheet::Type::none:
        return "none";
    case SampleSheet::Type::barcode:
        return "barcode";
    }
    throw std::runtime_error("Invalid enum dorado::utils::SampleSheet::Type value");
}

namespace details {
EolFileFormat get_eol_file_format(std::istream& input) {
    //    linux_eol,      '\n'
    //    windows_eol,    '\r` + '\n'
    //    osx_eol,        '\r'    (older osx style)
    auto postcondition_stream_is_reset = PostCondition([&input] { input.seekg(0); });
    auto result{EolFileFormat::linux_eol};
    std::size_t pos{0};
    input.seekg(0);
    auto character = input.peek();
    while (character != std::istream::traits_type::eof()) {
        if (character == '\n') {
            return EolFileFormat::linux_eol;
        }
        if (character == '\r') {
            input.seekg(pos + 1);
            character = input.peek();
            if (character == '\n') {
                return EolFileFormat::windows_eol;
            } else {
                return EolFileFormat::osx_eol;
            }
        }
        input.seekg(++pos);
        character = input.peek();
    }
    return result;
}

}  // namespace details

}  // namespace dorado::utils
