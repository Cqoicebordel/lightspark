/**************************************************************************
    Lightspark, a free flash player implementation

    Copyright (C) 2009-2013  Alessandro Pignotti (a.pignotti@sssup.it)

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
**************************************************************************/

#include "scripting/flash/globalization/stringtools.h"
#include "scripting/class.h"
#include "scripting/argconv.h"

#include <iostream>
#include <algorithm>

using namespace lightspark;

StringTools::StringTools(Class_base* c):
	ASObject(c)
{
}

void StringTools::sinit(Class_base* c)
{
	CLASS_SETUP(c, ASObject, _constructor, CLASS_SEALED|CLASS_FINAL);

	REGISTER_GETTER(c, actualLocaleIDName);
	REGISTER_GETTER(c, lastOperationStatus);
	REGISTER_GETTER(c, requestedLocaleIDName);

	c->setDeclaredMethodByQName("getAvailableLocaleIDNames","",Class<IFunction>::getFunction(c->getSystemState(),getAvailableLocaleIDNames),NORMAL_METHOD,true);
	c->setDeclaredMethodByQName("toLowerCase","",Class<IFunction>::getFunction(c->getSystemState(),toLowerCase),NORMAL_METHOD,true);
	c->setDeclaredMethodByQName("toUpperCase","",Class<IFunction>::getFunction(c->getSystemState(),toUpperCase),NORMAL_METHOD,true);
}

ASFUNCTIONBODY_ATOM(StringTools,_constructor)
{
	StringTools* th =asAtomHandler::as<StringTools>(obj);

	ARG_UNPACK_ATOM(th->requestedLocaleIDName);
	try
	{
		th->currlocale = std::locale(th->requestedLocaleIDName.raw_buf());
		th->actualLocaleIDName = th->requestedLocaleIDName;
		th->lastOperationStatus="noError";
	}
	catch (std::runtime_error& e)
	{
		uint32_t pos = th->requestedLocaleIDName.find("-");
		if(pos != tiny_string::npos)
		{
			tiny_string r("_");
			tiny_string l = th->requestedLocaleIDName.replace(pos,1,r);
			try
			{
				// try with "_" instead of "-"
				th->currlocale = std::locale(l.raw_buf());
				th->actualLocaleIDName = th->requestedLocaleIDName;
				th->lastOperationStatus="noError";
			}
			catch (std::runtime_error& e)
			{
				try
				{
					// try appending ".UTF-8"
					l += ".UTF-8";
					th->currlocale = std::locale(l.raw_buf());
					th->actualLocaleIDName = th->requestedLocaleIDName;
					th->lastOperationStatus="noError";
				}
				catch (std::runtime_error& e)
				{
					th->lastOperationStatus="usingDefaultWarning";
					LOG(LOG_ERROR,"unknown locale:"<<th->requestedLocaleIDName<<" "<<e.what());
				}
			}
		}
		else
		{
			try
			{
				// try appending ".UTF-8"
				th->requestedLocaleIDName += ".UTF-8";
				th->currlocale = std::locale(th->requestedLocaleIDName.raw_buf());
				th->actualLocaleIDName = th->requestedLocaleIDName;
				th->lastOperationStatus="noError";
			}
			catch (std::runtime_error& e)
			{
				th->lastOperationStatus="usingDefaultWarning";
				LOG(LOG_ERROR,"unknown locale:"<<th->requestedLocaleIDName<<" "<<e.what());
			}
		}
	}
}

ASFUNCTIONBODY_GETTER(StringTools, actualLocaleIDName);
ASFUNCTIONBODY_GETTER(StringTools, lastOperationStatus);
ASFUNCTIONBODY_GETTER(StringTools, requestedLocaleIDName);

ASFUNCTIONBODY_ATOM(StringTools,getAvailableLocaleIDNames)
{
  LOG(LOG_NOT_IMPLEMENTED,"StringTools.getAvailableLocaleIDNames is not implemented");
}

ASFUNCTIONBODY_ATOM(StringTools,toLowerCase)
{
  LOG(LOG_NOT_IMPLEMENTED,"StringTools.toLowerCase is not really tested for all formats");
  StringTools* th =asAtomHandler::as<StringTools>(obj);
  try
  {
    tiny_string s;
    ARG_UNPACK_ATOM(s);
    std::locale l =  std::locale::global(th->currlocale);
    std::string res = s.raw_buf();

    // TODO: tolower needs to be replaced with
    // something that matches Flash's tolower method better.
    // So "ÃŸ" here will not lower to "ãÿ" for example.
    transform(res.begin(), res.end(), res.begin(), ::tolower);
    std::locale::global(l);
    th->lastOperationStatus = "noError";
    ret = asAtomHandler::fromString(sys,res);
  }
  catch (std::runtime_error& e)
  {
    th->lastOperationStatus="usingDefaultWarning";
    LOG(LOG_ERROR,"unknown locale:"<<th->requestedLocaleIDName<<" "<<e.what());
  }
}

ASFUNCTIONBODY_ATOM(StringTools,toUpperCase)
{
  LOG(LOG_NOT_IMPLEMENTED,"StringTools.toUpperCase is not really tested for all formats");
  StringTools* th =asAtomHandler::as<StringTools>(obj);
  try
  {
    tiny_string s;
    ARG_UNPACK_ATOM(s);
    std::locale l =  std::locale::global(th->currlocale);
    std::string res = s.raw_buf();

    // TODO: toupper needs to be replaced with
    // something that matches Flash's toupper method better.
    transform(res.begin(), res.end(), res.begin(), ::toupper);
    std::locale::global(l);
    th->lastOperationStatus = "noError";
    ret = asAtomHandler::fromString(sys,res);
  }
  catch (std::runtime_error& e)
  {
    th->lastOperationStatus="usingDefaultWarning";
    LOG(LOG_ERROR,"unknown locale:"<<th->requestedLocaleIDName<<" "<<e.what());
  }
}
