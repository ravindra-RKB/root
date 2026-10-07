/// \file RLogger.cxx
/// \author Axel Naumann <axel@cern.ch>
/// \date 2015-07-07
/// \warning This is part of the ROOT 7 prototype! It will change without notice. It might trigger earthquakes. Feedback
/// is welcome!

/*************************************************************************
 * Copyright (C) 1995-2020, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#include "ROOT/RLogger.hxx"

#include "TError.h"

#include <algorithm>
#include <array>
#include <memory>
#include <vector>

// pin vtable
ROOT::RLogHandler::~RLogHandler() {}

namespace {
class RLogHandlerDefault : public ROOT::RLogHandler {
public:
   // Returns false if further emission of this log entry should be suppressed.
   bool Emit(const ROOT::RLogEntry &entry) override;
};

inline bool RLogHandlerDefault::Emit(const ROOT::RLogEntry &entry)
{
   constexpr static int numLevels = static_cast<int>(ROOT::ELogLevel::kDebug) + 1;
   int cappedLevel = std::min(static_cast<int>(entry.fLevel), numLevels - 1);
   constexpr static std::array<const char *, numLevels> sTag{
      {"{unset-error-level please report}", "FATAL", "Error", "Warning", "Info", "Debug"}};

   std::stringstream strm;
   auto channel = entry.fChannel;
   if (channel && !channel->GetName().empty())
      strm << '[' << channel->GetName() << "] ";
   strm << sTag[cappedLevel];

   if (!entry.fLocation.fFile.empty())
      strm << " " << entry.fLocation.fFile << ':' << entry.fLocation.fLine;
   if (!entry.fLocation.fFuncName.empty())
      strm << " in " << entry.fLocation.fFuncName;

   static constexpr const int errorLevelOld[] = {kFatal /*unset*/, kFatal, kError, kWarning, kInfo, kInfo /*debug*/};
   (*::GetErrorHandler())(errorLevelOld[cappedLevel], entry.fLevel == ROOT::ELogLevel::kFatal, strm.str().c_str(),
                          entry.fMessage.c_str());
   return true;
}
} // unnamed namespace

#include <cstdlib>

namespace ROOT {
namespace Internal {

ELogLevel GetEnvVerbosity(const std::string &channelName)
{
   if (channelName.empty())
      return ELogLevel::kUnset;
      
   const char *env = std::getenv("ROOT_LOG");
   if (!env)
      return ELogLevel::kUnset;

   std::string envStr(env);
   std::string search = channelName + "=";
   size_t pos = envStr.find(search);
   while (pos != std::string::npos) {
      if (pos == 0 || envStr[pos - 1] == ',') {
         size_t valPos = pos + search.length();
         size_t endPos = envStr.find(',', valPos);
         std::string valStr = envStr.substr(valPos, endPos - valPos);
         
         if (valStr == "Fatal") return ELogLevel::kFatal;
         if (valStr == "Error") return ELogLevel::kError;
         if (valStr == "Warning") return ELogLevel::kWarning;
         if (valStr == "Info") return ELogLevel::kInfo;
         if (valStr.find("Debug") == 0) {
            if (valStr.length() > 5 && valStr[5] == '(' && valStr.back() == ')') {
               try {
                  int level = std::stoi(valStr.substr(6, valStr.length() - 7));
                  return ELogLevel::kDebug + level;
               } catch (...) {}
            }
            return ELogLevel::kDebug;
         }
      }
      pos = envStr.find(search, pos + 1);
   }
   return ELogLevel::kUnset;
}

} // namespace Internal
} // namespace ROOT

ROOT::RLogManager &ROOT::RLogManager::Get()
{
   static RLogManager instance(std::make_unique<RLogHandlerDefault>());
   return instance;
}

std::unique_ptr<ROOT::RLogHandler> ROOT::RLogManager::Remove(RLogHandler *handler)
{
   auto iter = std::find_if(fHandlers.begin(), fHandlers.end(), [&](const std::unique_ptr<RLogHandler> &handlerPtr) {
      return handlerPtr.get() == handler;
   });
   if (iter != fHandlers.end()) {
      std::unique_ptr<RLogHandler> ret;
      swap(*iter, ret);
      fHandlers.erase(iter);
      return ret;
   }
   return {};
}

bool ROOT::RLogManager::Emit(const ROOT::RLogEntry &entry)
{
   auto channel = entry.fChannel;

   Increment(entry.fLevel);
   if (channel != this)
      channel->Increment(entry.fLevel);

   // Is there a specific level for the channel? If so, take that,
   // overruling the global one.
   if (channel->GetEffectiveVerbosity(*this) < entry.fLevel)
      return true;

   // Lock-protected extraction of handlers, such that they don't get added during the
   // handler iteration.
   std::vector<RLogHandler *> handlers;

   {
      std::lock_guard<std::mutex> lock(fMutex);

      handlers.resize(fHandlers.size());
      std::transform(fHandlers.begin(), fHandlers.end(), handlers.begin(),
                     [](const std::unique_ptr<RLogHandler> &handlerUPtr) { return handlerUPtr.get(); });
   }

   for (auto &&handler : handlers)
      if (!handler->Emit(entry))
         return false;
   return true;
}
