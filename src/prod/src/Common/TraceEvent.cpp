// ------------------------------------------------------------
// Copyright (c) Microsoft Corporation.  All rights reserved.
// Licensed under the MIT License (MIT). See License.txt in the repo root for license information.
// ------------------------------------------------------------

#include "stdafx.h"
#if defined(PLATFORM_UNIX)
#include "retail/native/FabricCommon/dll/TraceWrapper.Linux.h"
#endif

using namespace std;

namespace Common
{
    int TraceEvent::SamplingCount = 0;
    uint16 TraceEvent::ContextSequenceId = 0;
    ULONGLONG TraceEvent::AdminChannelKeywordMask = 0x8000000000000000;
    ULONGLONG TraceEvent::OperationalChannelKeywordMask = 0x4000000000000000;
    ULONGLONG TraceEvent::AnalyticChannelKeywordMask = 0x2000000000000000;
    ULONGLONG TraceEvent::DebugChannelKeywordMask = 0x1000000000000000;

    ULONGLONG TraceEvent::InitializeTestKeyword()
    {
        std::wstring testKeyword;
        FabricEnvironment::GetFabricTracesTestKeyword(testKeyword);
        ULONGLONG testKeywordValue = TraceKeywords::ParseTestKeyword(testKeyword);
        return testKeywordValue;
    }

    TraceEvent::TraceEvent(
        TraceTaskCodes::Enum taskId,
        StringLiteral taskName,
        USHORT eventId,
        StringLiteral eventName,
        LogLevel::Enum level,
        TraceChannelType::Enum channel,
        TraceKeywords::Enum keywords,
        StringLiteral format,
        ::REGHANDLE etwHandle,
        TraceSinkFilter & consoleFilter)
        :   fields_(),
            taskName_(taskName),
            eventName_(eventName),
            format_(format),
            etwFormat_(format.begin()),
            level_(level),
            hasId_(false),
            isChildEvent_(false),
            isText_(eventId < TextEvents),
            samplingRatio_(0),
            etwHandle_(etwHandle),
            consoleFilter_(consoleFilter),
            testKeyword_(InitializeTestKeyword())
    {
        if (eventName.size() == 0 || eventId >= (PerTaskEvents) || level < LogLevel::Critical || level > LogLevel::Noise 
            || channel < TraceChannelType::Admin || channel > TraceChannelType::Debug)
        {
            throw runtime_error("Invalid TraceEvent parameters");
        }

        // We need to apply a keyword mask to the keyword in order for it to show up
        // in the Windows Event Log. The mask that is applied depends on the channel
        // in which we want the event to show up.
        //
        // TODO: We are currently hard-coding the mask values, but this is not ideal 
        // because we have reverse-engineered these values by looking at the header 
        // file generated by mc.exe. The algorithm to determine these values is an 
        // internal implementation detail of mc.exe. A better alternative would be
        // to get the mask value from the source file generated by mc.exe, instead
        // of hard-coding it as a constant here.
        ULONGLONG keywordMask = 0;
        switch (channel)
        {
        case TraceChannelType::Admin:
            keywordMask = TraceEvent::AdminChannelKeywordMask;
            break;
        case TraceChannelType::Operational:
            keywordMask = TraceEvent::OperationalChannelKeywordMask;
            break;
        case TraceChannelType::Analytic:
            keywordMask = TraceEvent::AnalyticChannelKeywordMask;
            break;
        case TraceChannelType::Debug:
            keywordMask = TraceEvent::DebugChannelKeywordMask;
            break;
        }

        EventDescCreate(&descriptor_, (taskId << PerTaskEventBits) + eventId, 0, static_cast<UCHAR>(channel), static_cast<UCHAR>(level), taskId, 0, (static_cast<ULONGLONG>(keywords) | keywordMask));

        descriptor_.Keyword = descriptor_.Keyword | testKeyword_;

        if (isText_)
        {
            size_t index;
            AddEventField<std::wstring>(GetIdFieldName(), index);
            AddEventField<StringLiteral>(GetTypeFieldName(), index);
            AddEventField<std::wstring>(GetTextFieldName(), index);
            
            ConvertEtwFormatString();
        }
    }

    void TraceEvent::AddFieldDescription(std::string const & name, StringLiteral inType, StringLiteral outType, StringLiteral mapName)
    {
        if (name.size() == 0)
        {
            throw runtime_error("Field name can not be empty");
        }

        if (fields_.size() == 0)
        {
            if (name == TraceEvent::GetIdFieldName())
            {
                hasId_ = true;
            }
            else if (name == TraceEvent::GetContextSequenceIdFieldName())
            {
                isChildEvent_ = true;
            }
        }

        if (fields_.size() >= MaxFieldsPerEvent)
        {
            throw runtime_error("Too many fields");
        }

        fields_.push_back(FieldDescription(name, inType, outType, mapName));
    }
    
    void TraceEvent::AddContextSequenceField(std::string const & name)
    {
        AddFieldDescription("contextSequenceId_" + name, "win:UInt16", "xs:unsignedShort");
    }

    size_t TraceEvent::CountArguments(std::string const & format)
    {
        size_t maxIndex = 0;

        for (size_t i = 0; i < MaxFieldsPerEvent; i++)
        {
            string arg = "{" + Common::formatString(i) + "}";
            string hexArg = "{" + Common::formatString(i) + ":x}";
            if (StringUtility::Contains(format, arg) || StringUtility::Contains(format, hexArg))
            {
                maxIndex = (i + 1);
            }
        }

        return maxIndex;
    }

    void UpdateArg(string & format, size_t oldIndex, size_t newIndex)
    {
        string arg = "{" + Common::formatString(oldIndex) + "}";
        string newArg = "{" + Common::formatString(newIndex) + "}";
        StringUtility::Replace(format, arg, newArg);

        string hexArg = "{" + Common::formatString(oldIndex) + ":x}";
        string newHexArg = "{" + Common::formatString(newIndex) + ":x}";
        StringUtility::Replace(format, hexArg, newHexArg);
    }

    void TraceEvent::ExpandArgument(std::string & format, std::string const & innerFormat, size_t & index)
    {
        if (innerFormat.size() == 0)
        {
            index++;
            return;
        }

        size_t currentCount = CountArguments(format);
        size_t innerCount = CountArguments(innerFormat);

        for (int i = static_cast<int>(currentCount) - 1; i > static_cast<int>(index); i--)
        {
            UpdateArg(format, i, i + innerCount - 1);
        }

        string newFormat = innerFormat;
        for (int i = static_cast<int>(innerCount) - 1; i >= 0; i--)
        {
            UpdateArg(newFormat, i, index + i);
        }

        string currentArg = "{" + Common::formatString(index) + "}";
        StringUtility::Replace(format, currentArg, newFormat);

        index += innerCount;
    }

    void TraceEvent::ConvertEtwFormatString()
    {
        for (size_t i = 0; i < MaxFieldsPerEvent; i++)
        {
            string arg = "{" + Common::formatString(i) + "}";
            string newArg = "%" + Common::formatString(i + 1);
            StringUtility::Replace(etwFormat_, arg, newArg);

            string hexArg = "{" + Common::formatString(i) + ":x}";
            if (StringUtility::Contains(etwFormat_, hexArg))
            {
                fields_[i].SetHexFormat();
                StringUtility::Replace(etwFormat_, hexArg, newArg);
            }
        }
    }

    void TraceEvent::RefreshFilterStates(TraceSinkType::Enum sinkType, TraceSinkFilter const & filter)
    {
        int samplingRatio;

        filterStates_[sinkType] = filter.StaticCheck(
            level_,
            static_cast<TraceTaskCodes::Enum>(descriptor_.Task),
            isText_ ? StringLiteral() : eventName_,
            samplingRatio);

        if (sinkType == TraceSinkType::ETW)
        {
            samplingRatio_ = samplingRatio;
        }
    }

    StringLiteral TraceEvent::GetLevelString() const
    {
        switch (descriptor_.Level)
        {
        case LogLevel::Critical:
            return "win:Critical";
        case LogLevel::Error:
            return "win:Error";
        case LogLevel::Warning:
            return "win:Warning";
        case LogLevel::Info:
            return "win:Informational";
        case LogLevel::Noise:
            return "win:Verbose";
        default:
            throw runtime_error("Invalid level");
        }
    }

    void TraceEvent::WriteTextEvent(StringLiteral type, wstring const & id, wstring const & text, bool useETW, bool useFile, bool useConsole)
    {
        if (useETW)
        {
#if !defined(PLATFORM_UNIX)
            TraceEventContext context(3);

            context.Write(id);
            context.Write(type);
            context.Write(text);

            Write(context.GetEvents());
#else
            string taskName;
            StringWriterA w1(taskName);
            w1.Write(taskName_);
            string eventName;
            StringWriterA w2(eventName);
            w2.Write(type);

            TraceWrapper(taskName.c_str(), eventName.c_str(), level_, (char *)id.c_str(), (char *)text.c_str());
#endif
        }

        if (useFile)
        {
            TraceTextFileSink::Write(taskName_, type, level_, id, text);
        }

        if (useConsole)
        {
            TraceConsoleSink::Write(level_, text);
        }
    }

    void TraceEvent::Write(PEVENT_DATA_DESCRIPTOR data)
    {
        if ((descriptor_.Keyword & TraceKeywords::ForQuery) != 0)
        {
            EVENT_DESCRIPTOR eventDesc = descriptor_;

            // If both the Default keyword and the ForQuery keyword bits are set, we 
            // write the event twice. Once for the default trace session and once again
            // for the special trace session for query. 
            // An alternative would be to write the event just once and direct it to 
            // both trace sessions by setting both keyword bits. However, this is not
            // desirable because ETW behavior is such that if one of the trace session
            // buffers is full, the event will not go to either trace session. Events
            // with the ForQuery keyword are considered important events and we want
            // to make sure they go to the special trace session even if the buffers
            // of the default trace session are full. Hence we write the event twice.
            if ((eventDesc.Keyword & TraceKeywords::Default) != 0)
            {
                // Write it to the default trace session
                eventDesc.Keyword = eventDesc.Keyword & (~TraceKeywords::ForQuery);
                EventWrite(etwHandle_, &eventDesc, static_cast<ULONG>(fields_.size()), data);
            }

            // Write it to the special trace session for query
            // We don't want this write going to any other session, so we make sure
            // the ForQuery bit is the only keyword bit that we set.
            eventDesc.Keyword = TraceKeywords::ForQuery | testKeyword_;
            EventWrite(etwHandle_, &eventDesc, static_cast<ULONG>(fields_.size()), data);
        }
        else
        {
            EventWrite(etwHandle_, &descriptor_, static_cast<ULONG>(fields_.size()), data);
        }
    }

    void TraceEvent::WriteToTextSinkInternal(std::wstring const & id, std::wstring const & data, bool useConsole, bool useFile, bool useETW)
    {
        if (useETW)
        {
#if defined(PLATFORM_UNIX)
            string taskName;
            StringWriterA w1(taskName);
            w1.Write(taskName_);
            string eventName;
            StringWriterA w2(eventName);
            w2.Write(eventName_);

            TraceWrapper(taskName.c_str(), eventName.c_str(), level_, (char *)id.c_str(), (char *)data.c_str());
#endif
        }
        if (useFile)
        {
            TraceTextFileSink::Write(taskName_, eventName_, level_, id, data);
        }

        if (useConsole)
        {
            TraceConsoleSink::Write(level_, data);
        }
    }

    void TraceEvent::WriteManifest(TraceManifestGenerator & manifest)
    {
        StringWriterA & eventsWriter = manifest.GetEventsWriter();

        eventsWriter << "          <event" << "\r\n";
        eventsWriter << "              channel=\"" << static_cast<TraceChannelType::Enum>(descriptor_.Channel) << "\"" << "\r\n";
        eventsWriter << "              level=\"" << GetLevelString() << "\"" << "\r\n";
        eventsWriter << "              message=\"" << manifest.StringResource(etwFormat_) << "\"" << "\r\n";
        eventsWriter << "              opcode=\"win:Info\"" << "\r\n";
        
        ULONGLONG keywordWithoutMask = descriptor_.Keyword & ~(TraceEvent::AdminChannelKeywordMask | TraceEvent::OperationalChannelKeywordMask | TraceEvent::AnalyticChannelKeywordMask | TraceEvent::DebugChannelKeywordMask);
        if (keywordWithoutMask != 0)
        {
            string keywordNames;
            StringWriterA keywordNamesWriter(keywordNames);
            ULONGLONG currentKeyword = 1;
            bool insertSpaceBeforeKeyword = false;
            while (currentKeyword > 0)
            {
                if ((keywordWithoutMask & currentKeyword) == currentKeyword)
                {
                    if (insertSpaceBeforeKeyword)
                    {
                        keywordNamesWriter.Write(" ");
                    }
                    keywordNamesWriter.Write(TraceProvider::GetKeywordString(currentKeyword));
                    insertSpaceBeforeKeyword = true;
                }

                currentKeyword <<= 1;
            }

            eventsWriter << "              keywords=\"" << keywordNames << "\"" << "\r\n";
        }

        eventsWriter << "              symbol=\"Event_" << taskName_ << "_" << eventName_ << "\"" << "\r\n";
        eventsWriter << "              task=\"" << taskName_ << "\"" << "\r\n";
        eventsWriter << "              template=\"ntid_" << descriptor_.Id << "\"" << "\r\n";
        eventsWriter << "              value=\""  << descriptor_.Id << "\"" << "\r\n";
        eventsWriter << "              />\r\n";

        StringWriterA & templateWriter = manifest.GetTemplateWriter();

        if (fields_.empty())
        {
            templateWriter << "          <template tid=\"ntid_" << descriptor_.Id << "\"/>\r\n";
        }
        else
        {
            templateWriter << "          <template tid=\"ntid_" << descriptor_.Id << "\">\r\n";
            for (auto it = fields_.begin(); it != fields_.end(); it++)
            {
                templateWriter << "            <data" << "\r\n";
                templateWriter << "                inType=\"" << it->InType << "\"" << "\r\n";
                templateWriter << "                name=\"" << it->Name  << "\"" << "\r\n";
                templateWriter << "                outType=\"" << it->OutType << "\"" << "\r\n";

                if (it->MapName.size() > 0)
                {
                    templateWriter << "                map=\"" << it->MapName << "\"" << "\r\n";
                }

                templateWriter << "                />\r\n";
            }
            templateWriter << "          </template>\r\n";
        }
    }

    void TraceEvent::FieldDescription::SetHexFormat()
    {
        if (InType == StringLiteral("win:Int32") || InType == StringLiteral("win:UInt32"))
        {
            InType = "win:HexInt32";
            OutType = "win:HexInt32";
        }
        else if (InType == StringLiteral("win:Int64") || InType == StringLiteral("win:UInt64"))
        {
            InType = "win:HexInt64";
            OutType = "win:HexInt64";
        }
    }
}
