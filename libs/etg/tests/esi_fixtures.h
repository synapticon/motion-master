#pragma once

#include <format>
#include <string>
#include <string_view>

// Scaffolding for the hand-written ESI fragments the parser and flattener tests are built from.
//
// A valid ESI needs ~20 lines of Vendor/Descriptions/Device wrapping before the first interesting
// element, and repeating that in fifty tests would bury the one line each test is actually about.
// These helpers supply the wrapping so a test body can be the fragment under test and nothing
// else.

namespace mm::etg::test {

/// Wraps a `<DataTypes>`/`<Objects>` fragment in a single-device ESI.
///
/// @param dictionaryXml The body of `<Dictionary>` — normally a `<DataTypes>` block followed by an
///                      `<Objects>` block.
/// @param deviceExtras  Extra children of `<Device>`, appended after `<Profile>` (e.g. `<Slots>`).
/// @param modulesXml    The body of `<Modules>`; when non-empty a `<Modules>` element is emitted.
/// @param mailboxXml    The `<Mailbox>` element, e.g. to set `CoE CompleteAccess="1"`.
inline std::string wrapDictionary(std::string_view dictionaryXml,
                                  std::string_view deviceExtras = "",
                                  std::string_view modulesXml = "",
                                  std::string_view mailboxXml = "") {
  return std::format(
      R"(<?xml version="1.0" encoding="UTF-8"?>
<EtherCATInfo Version="1.2">
  <Vendor><Id>#x000022D2</Id><Name>Test Vendor</Name></Vendor>
  <Descriptions>
    <Devices>
      <Device Physics="YY">
        <Type ProductCode="#x00000201" RevisionNo="#x00010000">Test Device</Type>
        <Name LcId="1033">Test Device</Name>
        <GroupType>Test</GroupType>
        {}
        <Profile>
          <ProfileNo>402</ProfileNo>
          <Dictionary>{}</Dictionary>
        </Profile>
        {}
      </Device>
    </Devices>
    {}
  </Descriptions>
</EtherCATInfo>)",
      mailboxXml, dictionaryXml, deviceExtras,
      modulesXml.empty() ? std::string{} : std::format("<Modules>{}</Modules>", modulesXml));
}

/// A `<Module>` carrying a dictionary, for the slot-merge tests.
inline std::string moduleWithDictionary(std::string_view moduleIdent, std::string_view name,
                                        std::string_view dictionaryXml) {
  return std::format(
      R"(<Module>
           <Type ModuleIdent="{}">{}</Type>
           <Name>{}</Name>
           <Profile><Dictionary>{}</Dictionary></Profile>
         </Module>)",
      moduleIdent, name, name, dictionaryXml);
}

/// The minimal set of primitive `<DataType>` declarations. An ESI declares even `UDINT` as a
/// DataType in every dictionary, so tests that reference one need it present.
inline constexpr std::string_view kPrimitiveDataTypes = R"(
  <DataType><Name>BOOL</Name><BitSize>1</BitSize></DataType>
  <DataType><Name>SINT</Name><BitSize>8</BitSize></DataType>
  <DataType><Name>USINT</Name><BitSize>8</BitSize></DataType>
  <DataType><Name>INT</Name><BitSize>16</BitSize></DataType>
  <DataType><Name>UINT</Name><BitSize>16</BitSize></DataType>
  <DataType><Name>DINT</Name><BitSize>32</BitSize></DataType>
  <DataType><Name>UDINT</Name><BitSize>32</BitSize></DataType>
  <DataType><Name>REAL</Name><BitSize>32</BitSize></DataType>
  <DataType><Name>WORD</Name><BitSize>16</BitSize></DataType>
)";

}  // namespace mm::etg::test
