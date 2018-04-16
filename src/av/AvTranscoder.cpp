/*
 * Copyright (C) 2015 Emeric Poupon
 *
 * This file is part of LMS.
 *
 * LMS is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * LMS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with LMS.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <atomic>
#include <mutex>

#include "utils/Path.hpp"
#include "utils/Logger.hpp"

#include "AvTranscoder.hpp"

namespace Av {

#define LMS_LOG_TRANSCODE(sev)	LMS_LOG(TRANSCODE, INFO) << "[" << _id << "] - "

struct EncodingInfo
{
	Encoding encoding;
	std::string mimetype;
	int id;
};

static std::vector<EncodingInfo> encodingInfos =
{
	{Encoding::MP3,	"audio/mp3", 0},
	{Encoding::OGA, "audio/ogg", 1},
	{Encoding::WEBMA, "audio/webm", 3},
	{Encoding::M4A, "audio/mp4", 5},
};

std::string encodingToMimetype(Encoding encoding)
{
	for (auto encodingInfo : encodingInfos)
	{
		if (encodingInfo.encoding == encoding)
			return encodingInfo.mimetype;
	}

	throw std::logic_error("encoding_to_mimetype failed!");
}

int encodingToInt(Encoding encoding)
{
	for (auto encodingInfo : encodingInfos)
	{
		if (encodingInfo.encoding == encoding)
			return encodingInfo.id;
	}

	throw std::logic_error("encoding_to_int failed!");
}

Encoding encodingFromInt(int encodingId)
{
	for (auto encodingInfo : encodingInfos)
	{
		if (encodingInfo.id == encodingId)
			return encodingInfo.encoding;
	}

	throw std::logic_error("encoding_from_int failed!");
}

// TODO, parametrize?
static const std::vector<std::string> execNames =
{
	"avconv",
	"ffmpeg",
};

static std::mutex		transcoderMutex;
static boost::filesystem::path	avConvPath = boost::filesystem::path();
static std::atomic<size_t>	globalId = {0};

void
Transcoder::init()
{
	for (std::string execName : execNames)
	{
		boost::filesystem::path p = searchExecPath(execName);
		if (!p.empty())
		{
			avConvPath = p;
			break;
		}
	}

	if (!avConvPath.empty())
		LMS_LOG(TRANSCODE, INFO) << "Using transcoder " << avConvPath.string();
	else
		throw std::runtime_error("Cannot find any transcoder binary!");
}

Transcoder::Transcoder(boost::filesystem::path filePath, TranscodeParameters parameters)
: _filePath(filePath),
  _parameters(parameters),
  _isComplete(false),
  _id(globalId++)
{

}

bool
Transcoder::start()
{
	if (!boost::filesystem::exists(_filePath))
		return false;
	else if (!boost::filesystem::is_regular( _filePath) )
		return false;

	LMS_LOG_TRANSCODE(INFO) << "Transcoding file '" << _filePath.string() << "'";

	std::vector<std::string> args;

	args.push_back(avConvPath.string());

	// Make sure we do not produce anything in the stderr output
	// in order not to block the whole forked process
	args.push_back("-loglevel");
	args.push_back("quiet");
	args.push_back("-nostdin");

	// input Offset
	if (_parameters.offset)
	{
		args.push_back("-ss");
		args.push_back(std::to_string((*_parameters.offset).count()));
	}

	// Input file
	args.push_back("-i");
	args.push_back(_filePath.string());

	// Output bitrates
	args.push_back("-b:a");
	args.push_back(std::to_string(_parameters.bitrate));

	// Stream mapping, if set
	if (_parameters.stream)
	{
		args.push_back("-map");
		args.push_back("0:" + std::to_string(*_parameters.stream));
	}

	// Strip metadata
	args.push_back("-map_metadata");
	args.push_back("-1");

	// Skip video flows (including covers)
	args.push_back("-vn");

	// Codecs and formats
	switch( _parameters.encoding)
	{
		case Encoding::MP3:
			args.push_back("-f");
			args.push_back("mp3");
			break;

		case Encoding::OGA:
			args.push_back("-acodec");
			args.push_back("libvorbis");
			args.push_back("-f");
			args.push_back("ogg");
			break;

		case Encoding::WEBMA:
			args.push_back("-codec:a");
			args.push_back("libvorbis");
			args.push_back("-f");
			args.push_back("webm");
			break;

		case Encoding::M4A:
			args.push_back("-acodec");
			args.push_back("aac");
			args.push_back("-f");
			args.push_back("mp4");
			args.push_back("-strict");
			args.push_back("experimental");
			break;

		default:
			return false;
	}

	args.push_back("pipe:1");

	LMS_LOG_TRANSCODE(INFO) << "Dumping args (" << args.size() << ")";
	for (std::string arg : args)
		LMS_LOG_TRANSCODE(DEBUG) << "Arg = '" << arg << "'";

	// make sure only one thread is executing this part of code
	{
		std::lock_guard<std::mutex> lock(transcoderMutex);

		_child = std::make_shared<redi::ipstream>();

		// Caution: stdin must have been closed before
		_child->open(avConvPath.string(), args);
		if (!_child->is_open())
		{
			LMS_LOG_TRANSCODE(DEBUG) << "Exec failed!";
			return false;
		}

		if (_child->out().eof())
		{
			LMS_LOG_TRANSCODE(DEBUG) << "Early end of file!";
			return false;
		}

	}
	LMS_LOG_TRANSCODE(DEBUG) << "Stream opened!";

	return true;
}

void
Transcoder::process(std::vector<unsigned char>& output, std::size_t maxSize)
{
	if (!_child || _isComplete)
		return;

	if (_child->out().fail())
	{
		LMS_LOG_TRANSCODE(DEBUG) << "Stdout FAILED 2";
	}

	if (_child->out().eof())
	{
		LMS_LOG_TRANSCODE(DEBUG) << "Stdout ENDED 2";
	}

	output.resize(maxSize);

	LMS_LOG_TRANSCODE(DEBUG) << "Reading up to " << output.size() << " bytes";

	//Read on the output stream
	_child->out().read(reinterpret_cast<char*>(&output[0]), maxSize);
	output.resize(_child->out().gcount());

	LMS_LOG_TRANSCODE(DEBUG) << "Read " << output.size() << " bytes";

	if (_child->out().fail())
	{
		LMS_LOG_TRANSCODE(DEBUG) << "Stdout FAILED";
	}

	if (_child->out().eof())
	{
		LMS_LOG_TRANSCODE(DEBUG) << "Stdout EOF!";
		_child->clear();

		_isComplete = true;
		_child.reset();
	}

	_total += output.size();

	LMS_LOG_TRANSCODE(DEBUG) << "nb bytes = " << output.size() << ", total = " << _total;
}

Transcoder::~Transcoder()
{
	LMS_LOG_TRANSCODE(DEBUG) << ", ~Transcoder called! Total produced bytes = " << _total;

	if (_child)
	{
		LMS_LOG_TRANSCODE(DEBUG) << "Child still here!";
		_child->rdbuf()->kill(SIGKILL);
		LMS_LOG_TRANSCODE(DEBUG) << "Closing...";
		_child->rdbuf()->close();
		LMS_LOG_TRANSCODE(DEBUG) << "Closing DONE";
	}
}

} // namespace Transcode
