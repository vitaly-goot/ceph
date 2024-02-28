/**
 * @file test_rgw_expat_mitigations.cc
 * @author André Lucas (alucas@akamai.com)
 * @brief Check for presence and operation of Expat 'Billion Laughs' attack
 * mitigations.
 * @version 0.1
 * @date 2024-02-12
 *
 * @copyright Copyright (c) 2024
 */
// XML_GE must be defined or the mitigations won't be declared in <expat.h>.
#define XML_GE 1

#include <iostream>

#include <expat.h>
#include <gtest/gtest.h>

#include <string.h>

namespace
{

    // These are local to this file, so we can set artifically low values for
    // testing.
    static constexpr unsigned long long threshold_bytes = 1024UL * 1024UL;
    static constexpr double amplification_factor = 10.0;

    // The original Billion Laughs attack. There's a good chance this will upset
    // code scanners.
    static std::string billion_laughs = R"""(<?xml version="1.0"?>
<!DOCTYPE lolz [
 <!ENTITY lol "lol">
 <!ELEMENT lolz (#PCDATA)>
 <!ENTITY lol1 "&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;">
 <!ENTITY lol2 "&lol1;&lol1;&lol1;&lol1;&lol1;&lol1;&lol1;&lol1;&lol1;&lol1;">
 <!ENTITY lol3 "&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;">
 <!ENTITY lol4 "&lol3;&lol3;&lol3;&lol3;&lol3;&lol3;&lol3;&lol3;&lol3;&lol3;">
 <!ENTITY lol5 "&lol4;&lol4;&lol4;&lol4;&lol4;&lol4;&lol4;&lol4;&lol4;&lol4;">
 <!ENTITY lol6 "&lol5;&lol5;&lol5;&lol5;&lol5;&lol5;&lol5;&lol5;&lol5;&lol5;">
 <!ENTITY lol7 "&lol6;&lol6;&lol6;&lol6;&lol6;&lol6;&lol6;&lol6;&lol6;&lol6;">
 <!ENTITY lol8 "&lol7;&lol7;&lol7;&lol7;&lol7;&lol7;&lol7;&lol7;&lol7;&lol7;">
 <!ENTITY lol9 "&lol8;&lol8;&lol8;&lol8;&lol8;&lol8;&lol8;&lol8;&lol8;&lol8;">
]>
<lolz>&lol9;</lolz>)""";

    // Less obnoxious but still objectionable. Should not parse.
    static std::string million_laughs = R"""(<?xml version="1.0"?>
<!DOCTYPE lolz [
 <!ENTITY lol "lol">
 <!ELEMENT lolz (#PCDATA)>
 <!ENTITY lol1 "&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;">
 <!ENTITY lol2 "&lol1;&lol1;&lol1;&lol1;&lol1;&lol1;&lol1;&lol1;&lol1;&lol1;">
 <!ENTITY lol3 "&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;">
 <!ENTITY lol4 "&lol3;&lol3;&lol3;&lol3;&lol3;&lol3;&lol3;&lol3;&lol3;&lol3;">
 <!ENTITY lol5 "&lol4;&lol4;&lol4;&lol4;&lol4;&lol4;&lol4;&lol4;&lol4;&lol4;">
 <!ENTITY lol6 "&lol5;&lol5;&lol5;&lol5;&lol5;&lol5;&lol5;&lol5;&lol5;&lol5;">
]>
<lolz>&lol6;</lolz>)""";

    // Should parse fine with reasonable settings.
    static std::string thousand_laughs = R"""(<?xml version="1.0"?>
<!DOCTYPE lolz [
 <!ENTITY lol "lol">
 <!ELEMENT lolz (#PCDATA)>
 <!ENTITY lol1 "&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;">
 <!ENTITY lol2 "&lol1;&lol1;&lol1;&lol1;&lol1;&lol1;&lol1;&lol1;&lol1;&lol1;">
 <!ENTITY lol3 "&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;">
]>
<lolz>&lol3;</lolz>)""";

    // The result is a tiny document and must parse.
    static std::string single_laugh = R"""(<?xml version="1.0"?>
<!DOCTYPE lolz [
 <!ENTITY lol "lol">
 <!ELEMENT lolz (#PCDATA)>
 <!ENTITY lol1 "&lol;">
]>
<lolz>&lol1;</lolz>
)""";

    TEST(RGWExpatMitigationsBuild, CompileAndLink)
    {
        // Make sure we can link, not just compile, with the XML mitigations. This
        // will fail to link if we're using old Expat.
        auto p = XML_ParserCreate(nullptr);
        ASSERT_TRUE(p != nullptr) << "Failed to create XML parser";
        ASSERT_EQ(XML_TRUE, XML_SetBillionLaughsAttackProtectionActivationThreshold(p, threshold_bytes))
            << "Failed to set XML parser attack protection threshold";
        ASSERT_EQ(XML_TRUE, XML_SetBillionLaughsAttackProtectionMaximumAmplification(p, amplification_factor))
            << "Failed to set XML parser attack protection amplification";
        XML_ParserFree(p);
    }

    class RAII_XMLParser
    {
    public:
        RAII_XMLParser()
            : parser_(XML_ParserCreate(nullptr))
        {
            if (parser_ == nullptr)
            {
                throw std::runtime_error("Failed to create XML parser");
            }
        }
        ~RAII_XMLParser() { XML_ParserFree(parser_); }
        XML_Parser get() const { return parser_; }

    private:
        XML_Parser parser_;
    }; // class RAII_XMLParser

    class RGWExpatMitigations : public ::testing::Test
    {
    protected:
        static void NullStartElement([[maybe_unused]] void *userData,
                                     [[maybe_unused]] const XML_Char *name,
                                     [[maybe_unused]] const XML_Char **atts){};
        static void NullEndElement([[maybe_unused]] void *userData,
                                   [[maybe_unused]] const XML_Char *name){};

        void DisplayError(const RAII_XMLParser &p)
        {
            std::cerr << "Error: " << XML_ErrorString(XML_GetErrorCode(p.get()))
                      << " at line " << XML_GetErrorLineNumber(p.get())
                      << " column " << XML_GetErrorColumnNumber(p.get())
                      << std::endl;
        }
    }; // class RGWExpatMitigations

    struct Att
    {
        const std::string *attack;
        std::string description;
    };

    // Attempt to parse known-bad XML and check for a specific failure mode.
    TEST_F(RGWExpatMitigations, AttacksReturnCorrectError)
    {

        std::vector<Att> attacks = {
            {&billion_laughs, "Billion Laughs"},
            {&million_laughs, "Million Laughs"},
        };

        for (const auto &att : attacks)
        {

            RAII_XMLParser p;
            ASSERT_EQ(XML_TRUE, XML_SetBillionLaughsAttackProtectionActivationThreshold(p.get(), threshold_bytes))
                << "Failed to set XML parser attack protection threshold";
            ASSERT_EQ(XML_TRUE,
                      XML_SetBillionLaughsAttackProtectionMaximumAmplification(
                          p.get(), amplification_factor))
                << "Failed to set XML parser attack protection amplification";

            XML_SetElementHandler(p.get(), NullStartElement, NullEndElement);

            void *const buf = XML_GetBuffer(p.get(), BUFSIZ);
            ASSERT_NE(buf, nullptr) << "Failed to allocate XML buffer";

            // std::cerr << att.description << ": " << *att.attack << std::endl;

            ASSERT_LT(att.attack->size(), BUFSIZ)
                << "Attack " << att.description << " doesn't fit into buffer";
            strncpy((char *)buf, att.attack->c_str(), BUFSIZ);

            auto res = XML_ParseBuffer(p.get(), (int)att.attack->size(), true);
            if (res == XML_STATUS_ERROR)
            {
                DisplayError(p);
            }
            ASSERT_EQ(res, XML_STATUS_ERROR) << "Attack " << att.description
                                             << " should not have parsed without error";
            ASSERT_EQ(XML_GetErrorCode(p.get()), XML_ERROR_AMPLIFICATION_LIMIT_BREACH)
                << "Attack " << att.description
                << " should have failed with an 'amplification limit breached' error, not '"
                << XML_ErrorString(XML_GetErrorCode(p.get())) << "'";
        }
    }

    // Attempt to parse reasonable XML (meaning: Doesn't consume huge amounts of
    // memory or CPU) and check for success.
    TEST_F(RGWExpatMitigations, NonAttacksWithEntityExpansionPass)
    {

        std::vector<Att> attacks = {
            {&single_laugh, "Single Laugh"},
            {&thousand_laughs, "Thousand Laughs"},
        };

        for (const auto &att : attacks)
        {

            RAII_XMLParser p;
            ASSERT_EQ(XML_TRUE, XML_SetBillionLaughsAttackProtectionActivationThreshold(p.get(), threshold_bytes))
                << "Failed to set XML parser attack protection threshold";
            ASSERT_EQ(XML_TRUE,
                      XML_SetBillionLaughsAttackProtectionMaximumAmplification(
                          p.get(), amplification_factor))
                << "Failed to set XML parser attack protection amplification";

            XML_SetElementHandler(p.get(), NullStartElement, NullEndElement);

            void *const buf = XML_GetBuffer(p.get(), BUFSIZ);
            ASSERT_NE(buf, nullptr) << "Failed to allocate XML buffer";

            // std::cerr << att.description << ": " << *att.attack << std::endl;
            ASSERT_LT(att.attack->size(), BUFSIZ)
                << "Attack " << att.description << " doesn't fit into buffer";
            strncpy((char *)buf, att.attack->c_str(), BUFSIZ);

            auto res = XML_ParseBuffer(p.get(), (int)att.attack->size(), true);
            if (res == XML_STATUS_ERROR)
            {
                DisplayError(p);
            }
            ASSERT_EQ(res, XML_STATUS_OK) << "Non-attack " << att.description
                                          << " failed to parse";
        }
    }

} // namespace