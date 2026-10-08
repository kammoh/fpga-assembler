#include "fpga/injected-features.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "fpga/database-parsers.h"
#include "fpga/database.h"
#include "gtest/gtest.h"

namespace fpga {
namespace {

constexpr char kLeftIob[] = "LIOB18_X1Y10";
constexpr char kLeftIoi[] = "LIOI_X2Y10";
constexpr char kRightIob[] = "RIOB18_X5Y10";
constexpr char kRightIoi[] = "RIOI_X4Y10";
constexpr char kDrive[] = "LVCMOS15_LVCMOS18.DRIVE.I12_I16_I2_I4_I6_I8";

Tile MakeTile(const std::string &type, uint32_t x, uint32_t y) {
  Tile tile;
  tile.type = type;
  tile.coord = {x, y};
  tile.bits = {{ConfigBusType::kCLBIOCLK, BitsBlock{.alias = {},
                                                    .base_address = 0x00421000,
                                                    .frames = 42,
                                                    .offset = 18,
                                                    .words = 2}}};
  return tile;
}

// A database of one HP IOB tile and the IOI tile beside it on each side of the
// device.  Only the output buffer glue of the IOB tiles is documented.
PartDatabase MakeDatabase() {
  const TileGrid grid = {
    {kLeftIob, MakeTile("LIOB18", 1, 10)},
    {kLeftIoi, MakeTile("LIOI", 2, 10)},
    {kRightIob, MakeTile("RIOB18", 5, 10)},
    {kRightIoi, MakeTile("RIOI", 4, 10)},
  };
  const SegmentsBits segbits = {
    {{"LIOB18.IOB_Y1.OBUF_HP_BANK_GLUE", 0}, {{0, 1, true}}},
    {{"RIOB18.IOB_Y1.OBUF_HP_BANK_GLUE", 0}, {{0, 1, true}}},
  };
  TileTypesSegmentsBitsGetter bits_getter =
    [segbits](const std::string &tile_type)
    -> std::optional<SegmentsBitsWithPseudoPIPs> {
    if (tile_type != "LIOB18" && tile_type != "RIOB18") {
      return std::nullopt;
    }
    return SegmentsBitsWithPseudoPIPs{
      .pips = {},
      .segment_bits = {{ConfigBusType::kCLBIOCLK, segbits}},
    };
  };
  absl::StatusOr<BanksTilesRegistry> banks =
    BanksTilesRegistry::Create(Part{}, {}, grid);
  return PartDatabase(std::make_shared<PartDatabase::Tiles>(
    grid, std::move(bits_getter), std::move(banks.value()), Part{}));
}

// Returns whether assembling the given FASM features adds the output buffer
// glue to the IOB_Y1 site of the given IOB tile.
bool AddsObufGlue(const std::string &iob_tile,
                  const std::vector<std::string> &feature_names) {
  PartDatabase db = MakeDatabase();
  std::vector<FasmFeature> features;
  for (const std::string &name : feature_names) {
    features.push_back(FasmFeature{
      .line = 1, .name = name, .start_bit = 0, .width = 1, .bits = 1});
  }
  InjectConfigurationFeatures(db, /*emit_pudc_b_pullup=*/false, features);
  for (const FasmFeature &feature : features) {
    if (feature.name == iob_tile + ".IOB_Y1.OBUF_HP_BANK_GLUE") {
      return true;
    }
  }
  return false;
}

std::string Drive(const std::string &iob_tile) {
  return iob_tile + ".IOB_Y1." + kDrive;
}

TEST(HpBankGlue, PlainOutputGetsTheGlue) {
  EXPECT_TRUE(AddsObufGlue(kLeftIob, {Drive(kLeftIob)}));
  EXPECT_TRUE(AddsObufGlue(kRightIob, {Drive(kRightIob)}));
}

TEST(HpBankGlue, OlogicWithOnlyPassThroughFeaturesGetsTheGlue) {
  // ZINV_T1 sets none of the glue bits, so they can only come from the glue.
  EXPECT_TRUE(AddsObufGlue(
    kLeftIob, {Drive(kLeftIob), std::string(kLeftIoi) + ".OLOGIC_Y1.ZINV_T1"}));
  EXPECT_TRUE(AddsObufGlue(
    kLeftIob, {Drive(kLeftIob), std::string(kLeftIoi) + ".OLOGIC_Y1.OQUSED",
               std::string(kLeftIoi) + ".OLOGIC_Y1.OMUX.D1",
               std::string(kLeftIoi) + ".OLOGIC_Y1.OSERDES.DATA_RATE_TQ.BUF"}));
}

TEST(HpBankGlue, OlogicHoldingAnOddrOnTheTristateGetsNoGlue) {
  EXPECT_FALSE(AddsObufGlue(
    kLeftIob, {Drive(kLeftIob), std::string(kLeftIoi) + ".OLOGIC_Y1.OQUSED",
               std::string(kLeftIoi) + ".OLOGIC_Y1.OMUX.D1",
               std::string(kLeftIoi) + ".OLOGIC_Y1.OSERDES.DATA_RATE_TQ.DDR",
               std::string(kLeftIoi) + ".OLOGIC_Y1.ZINV_T2"}));
}

TEST(HpBankGlue, OlogicHoldingAnOddrOnTheDataGetsNoGlue) {
  EXPECT_FALSE(AddsObufGlue(
    kLeftIob,
    {Drive(kLeftIob), std::string(kLeftIoi) + ".OLOGIC_Y1.ODDR_TDDR.IN_USE",
     std::string(kLeftIoi) + ".OLOGIC_Y1.OQUSED",
     std::string(kLeftIoi) + ".OLOGIC_Y1.OSERDES.DATA_RATE_OQ.DDR",
     std::string(kLeftIoi) + ".OLOGIC_Y1.OSERDES.DATA_RATE_TQ.BUF"}));
}

TEST(HpBankGlue, OlogicOnTheRightColumnIsBesideTheIobToItsLeft) {
  EXPECT_FALSE(AddsObufGlue(
    kRightIob, {Drive(kRightIob), std::string(kRightIoi) +
                                    ".OLOGIC_Y1.OSERDES.DATA_RATE_OQ.DDR"}));
}

TEST(HpBankGlue, OnlyTheOlogicOfTheSameSiteCounts) {
  // The cell is in OLOGIC_Y0, the output is on IOB_Y1.
  EXPECT_TRUE(AddsObufGlue(
    kLeftIob, {Drive(kLeftIob),
               std::string(kLeftIoi) + ".OLOGIC_Y0.OSERDES.DATA_RATE_OQ.DDR"}));
}

TEST(HpBankGlue, OnlyTheOlogicOfTheNeighboringTileCounts) {
  // The RIOI tile is on the other side of the device.
  EXPECT_TRUE(AddsObufGlue(
    kLeftIob, {Drive(kLeftIob), std::string(kRightIoi) +
                                  ".OLOGIC_Y1.OSERDES.DATA_RATE_OQ.DDR"}));
}

TEST(HpBankGlue, ClearedOlogicFeatureIsNotACell) {
  PartDatabase db = MakeDatabase();
  std::vector<FasmFeature> features = {
    {1, Drive(kLeftIob), 0, 1, 1},
    {2, std::string(kLeftIoi) + ".OLOGIC_Y1.OSERDES.DATA_RATE_OQ.DDR", 0, 1, 0},
  };
  InjectConfigurationFeatures(db, /*emit_pudc_b_pullup=*/false, features);
  EXPECT_EQ(features.back().name,
            std::string(kLeftIob) + ".IOB_Y1.OBUF_HP_BANK_GLUE");
}

}  // namespace
}  // namespace fpga
