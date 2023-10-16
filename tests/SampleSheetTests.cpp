/*
 * sample_sheet_test.cpp
 *
 *  Created on: August 8, 2022.
 *  Proprietary and confidential information of Oxford Nanopore Technologies, Limited
 *  All rights reserved; (c)2022: Oxford Nanopore Technologies, Limited
 */

#include "utils/SampleSheet.h"

#include "TestUtils.h"

#include <catch2/catch.hpp>

#include <set>
#include <sstream>
#include <string>
#include <vector>

#define CUT_TAG "[sample_sheets]"

TEST_CASE(CUT_TAG " load valid no-barcode sample sheet", CUT_TAG) {
    dorado::utils::SampleSheet sample_sheet;
    auto no_barcode_filename = get_sample_sheets_data_dir() / "no_barcode.csv";
    REQUIRE_NOTHROW(sample_sheet.load(no_barcode_filename.string()));
    CHECK(sample_sheet.get_type() == dorado::utils::SampleSheet::Type::none);

    // Test that all the alias functions return empty strings
    std::string alias;
    REQUIRE_NOTHROW(
            alias = sample_sheet.get_alias("FA026858", "pos_id", "sequencing_20200522", "BC10"));
    CHECK(alias == "");
}

TEST_CASE(CUT_TAG " load valid single barcode sample sheet", CUT_TAG) {
    dorado::utils::SampleSheet sample_sheet;
    auto single_barcode_filename = get_sample_sheets_data_dir() / "single_barcode.csv";
    REQUIRE_NOTHROW(sample_sheet.load(single_barcode_filename.string()));
    CHECK(sample_sheet.get_type() == dorado::utils::SampleSheet::Type::barcode);

    // Test first entry loads correctly
    std::string alias;
    REQUIRE_NOTHROW(alias = sample_sheet.get_alias("FA026858", "", "sequencing_20200522", "BC01"));
    CHECK(alias == "patient_id_5");

    // Test last entry loads correctly
    REQUIRE_NOTHROW(alias = sample_sheet.get_alias("FA026858", "", "sequencing_20200522", "BC08"));
    CHECK(alias == "patient_id_4");

    // TODO: is this what we want?
    // Test that asking for position_id when it's not there stops you getting an alias
    REQUIRE_NOTHROW(
            alias = sample_sheet.get_alias("FA026858", "pos_id", "sequencing_20200522", "BC01"));
    CHECK(alias == "");

    // Test that asking for neither position_id or flowcell_id stops you getting an alias
    REQUIRE_NOTHROW(alias = sample_sheet.get_alias("", "", "sequencing_20200522", "BC01"));
    CHECK(alias == "");

    // Test non-existent entry
    REQUIRE_NOTHROW(alias = sample_sheet.get_alias("FA026858", "", "sequencing_20200522", "BC10"));
    CHECK(alias == "");
}

TEST_CASE(CUT_TAG " load valid single barcode sample sheet with unique mapping", CUT_TAG) {
    dorado::utils::SampleSheet sample_sheet("", true);

    // single barcode contains info for 1 flow cell, 1 experiment - all barcodes are uniquely mapped
    auto single_barcode_filename = get_sample_sheets_data_dir() / "single_barcode.csv";
    REQUIRE_NOTHROW(sample_sheet.load(single_barcode_filename.string()));
    REQUIRE(sample_sheet.get_type() == dorado::utils::SampleSheet::Type::barcode);

    // Test entries load correctly without flow_cell_id or experiment_id info

    // Test first entry loads correctly
    std::string alias;
    REQUIRE_NOTHROW(alias = sample_sheet.get_alias("", "", "", "BC01"));
    CHECK(alias == "patient_id_5");

    // Test last entry loads correctly
    REQUIRE_NOTHROW(alias = sample_sheet.get_alias("", "", "", "BC08"));
    CHECK(alias == "patient_id_4");

    // Test non-existent entry
    REQUIRE_NOTHROW(alias = sample_sheet.get_alias("", "", "", "BC10"));
    CHECK(alias == "");
}

TEST_CASE(CUT_TAG " load sample sheet cross platform (parameterised)", CUT_TAG) {
    // Not using files from the data folder as there is a CI check that all files conform
    // to linux file endings
    const auto eol_chars = GENERATE(as<std::string>{},
                                    "\n",    // linux style
                                    "\r\n",  // windows style
                                    "\r"     // osx style);
    );
    CAPTURE(eol_chars);
    const std::string HEADER_LINE{"flow_cell_id,kit,sample_id,experiment_id,barcode,alias,type"};
    const std::string RECORD_LINE{
            "FA026858,SQK-RBK004,barcoding_run,sequencing_20200522,BC01,patient_id_5,test_"
            "sample"};
    dorado::utils::SampleSheet sample_sheet{};
    std::stringstream input_file{HEADER_LINE + eol_chars + RECORD_LINE + eol_chars};

    sample_sheet.load(input_file, "TEST_GENERATED_INPUT_STREAM");

    REQUIRE(sample_sheet.get_type() == dorado::utils::SampleSheet::Type::barcode);

    std::string alias;
    REQUIRE_NOTHROW(alias = sample_sheet.get_alias("FA026858", "", "sequencing_20200522", "BC01"));
    CHECK(alias == "patient_id_5");
}

TEST_CASE(CUT_TAG " load odd-but-valid test sample sheet", CUT_TAG) {
    dorado::utils::SampleSheet sample_sheet;
    auto odd_valid_filename = get_sample_sheets_data_dir() / "valid_but_weird.csv";
    REQUIRE_NOTHROW(sample_sheet.load(odd_valid_filename.string()));
}

TEST_CASE(CUT_TAG " load non-existent test sample sheet", CUT_TAG) {
    dorado::utils::SampleSheet sample_sheet;
    auto non_existant_filename = get_sample_sheets_data_dir() / "ovenchips.csv";
    REQUIRE_THROWS(sample_sheet.load(non_existant_filename.string()));
}

TEST_CASE(CUT_TAG " load file with invalid alias", CUT_TAG) {
    dorado::utils::SampleSheet sample_sheet;
    auto sample_sheet_filename = get_sample_sheets_data_dir() / "invalid1.csv";
    REQUIRE_THROWS(sample_sheet.load(sample_sheet_filename.string()));
}

TEST_CASE(CUT_TAG " get_eol_file_format with valid stream does not throw", CUT_TAG) {
    std::stringstream input{"blah"};
    REQUIRE_NOTHROW(dorado::utils::details::get_eol_file_format(input));
}

using dorado::utils::details::EolFileFormat;

struct EolTestCase {
    std::string input;
    EolFileFormat expected_format;
};

TEST_CASE(CUT_TAG " get_eol_file_format parameterised", CUT_TAG) {
    auto [input, expected_format] = GENERATE(table<std::string, EolFileFormat>(
            {std::make_tuple("first\nsecond", EolFileFormat::linux_eol),
             std::make_tuple("first\rsecond", EolFileFormat::osx_eol),
             std::make_tuple("first\r\nsecond", EolFileFormat::windows_eol),
             std::make_tuple("", EolFileFormat::linux_eol),
             std::make_tuple("no end of line characters", EolFileFormat::linux_eol),
             std::make_tuple("first\n", EolFileFormat::linux_eol),
             std::make_tuple("first\r", EolFileFormat::osx_eol),
             std::make_tuple("first\r\n", EolFileFormat::windows_eol),
             std::make_tuple("\nsecond", EolFileFormat::linux_eol),
             std::make_tuple("\rsecond", EolFileFormat::osx_eol),
             std::make_tuple("\r\nsecond", EolFileFormat::windows_eol)}));
    CAPTURE(input);
    std::stringstream input_stream{input};
    CHECK(expected_format == dorado::utils::details::get_eol_file_format(input_stream));
}

TEST_CASE(CUT_TAG " get_eol_file_format sets stream pos to start", CUT_TAG) {
    std::stringstream input{"first\nsecond"};
    CHECK(dorado::utils::details::get_eol_file_format(input) == EolFileFormat::linux_eol);
    std::string first;
    input >> first;
    CHECK(first == "first");
}

TEST_CASE(CUT_TAG " barcode values", CUT_TAG) {
    const size_t num_rows = 20;
    std::stringstream input_file;
    std::vector<std::string> expected;

    // ID to string using the %02i convention
    auto to_str = [](size_t i) {
        char result[16]{};
        snprintf(result, 16, "%02zu", i);
        return std::string(result);
    };

    input_file << "flow_cell_id,kit,experiment_id,barcode,alias\n";
    for (size_t i = 0; i < num_rows; i++) {
        const auto barcode = "BC" + to_str(i);
        input_file << "id,kit,expr," << barcode << ",patient_" << i << "\n";
        expected.push_back(barcode);
    }

    // Load it
    dorado::utils::SampleSheet sample_sheet;
    REQUIRE_NOTHROW(sample_sheet.load(input_file, "barcode values test"));
    REQUIRE(sample_sheet.get_type() == dorado::utils::SampleSheet::Type::barcode);

    // Grab the barcodes in the CSV
    const auto barcodes = sample_sheet.get_barcode_values();
    REQUIRE(barcodes.has_value());

    // Check that they're equal
    REQUIRE(barcodes->size() == num_rows);
    REQUIRE(expected.size() == num_rows);
    REQUIRE(std::is_permutation(barcodes->begin(), barcodes->end(), expected.begin()));
}
