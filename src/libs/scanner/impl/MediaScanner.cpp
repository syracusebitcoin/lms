/*
 * Copyright (C) 2013 Emeric Poupon
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

#include "MediaScanner.hpp"

#include <boost/asio/placeholders.hpp>

#include <Wt/WLocalDateTime.h>

#include "database/Artist.hpp"
#include "database/Cluster.hpp"
#include "database/Release.hpp"
#include "database/ScanSettings.hpp"
#include "database/Track.hpp"
#include "database/TrackFeatures.hpp"
#include "metadata/TagLibParser.hpp"
#include "utils/Exception.hpp"
#include "utils/Logger.hpp"
#include "utils/Path.hpp"
#include "utils/UUID.hpp"
#include "AcousticBrainzUtils.hpp"

using namespace Database;

namespace {

Wt::WDate
getNextMonday(Wt::WDate current)
{
	do
	{
		current = current.addDays(1);
	} while (current.dayOfWeek() != 1);

	return current;
}

Wt::WDate
getNextFirstOfMonth(Wt::WDate current)
{
	do
	{
		current = current.addDays(1);
	} while (current.day() != 1);

	return current;
}

bool
isFileSupported(const std::filesystem::path& file, const std::unordered_set<std::filesystem::path>& extensions)
{
	const std::filesystem::path extension {StringUtils::stringToLower(file.extension().string())};

	return (extensions.find(extension) != extensions.end());
}

bool
isPathInParentPath(const std::filesystem::path& path, const std::filesystem::path& parentPath)
{
	std::filesystem::path curPath = path;

	while (curPath.parent_path() != curPath)
	{
		curPath = curPath.parent_path();

		if (curPath == parentPath)
			return true;
	}

	return false;
}

static
Artist::pointer
createArtist(Session& session, const MetaData::Artist& artistInfo)
{
	Artist::pointer artist {Artist::create(session, artistInfo.name)};

	if (artistInfo.musicBrainzArtistID)
		artist.modify()->setMBID(*artistInfo.musicBrainzArtistID);
	if (artistInfo.sortName)
		artist.modify()->setSortName(*artistInfo.sortName);

	return artist;
}

static
void
updateArtistIfNeeded(const Artist::pointer& artist, const MetaData::Artist& artistInfo)
{
	// Name may have been updated
	if (artist->getName() != artistInfo.name)
	{
		artist.modify()->setName(artistInfo.name);
	}

	// Sortname may have been updated
	if (artistInfo.sortName && *artistInfo.sortName != artist->getSortName() )
	{
		artist.modify()->setSortName(*artistInfo.sortName);
	}
}

std::vector<Artist::pointer>
getOrCreateArtists(Session& session, const std::vector<MetaData::Artist>& artistsInfo)
{
	std::vector<Artist::pointer> artists;

	for (const MetaData::Artist& artistInfo : artistsInfo)
	{
		Artist::pointer artist;

		// First try to get by MBID
		if (artistInfo.musicBrainzArtistID)
		{
			artist = Artist::getByMBID(session, *artistInfo.musicBrainzArtistID);
			if (!artist)
				artist = createArtist(session, artistInfo);
			else
				updateArtistIfNeeded(artist, artistInfo);

			artists.emplace_back(std::move(artist));
			continue;
		}

		// Fall back on artist name (collisions may occur)
		if (!artistInfo.name.empty())
		{
			for (const Artist::pointer& sameNamedArtist : Artist::getByName(session, artistInfo.name))
			{
				// Do not fallback on artist that is correctly tagged
				if (!sameNamedArtist->getMBID())
				{
					artist = sameNamedArtist;
					break;
				}
			}

			// No Artist found with the same name and without MBID -> creating
			if (!artist)
				artist = createArtist(session, artistInfo);
			else
				updateArtistIfNeeded(artist, artistInfo);

			artists.emplace_back(std::move(artist));
			continue;
		}
	}

	return artists;
}

Release::pointer
getOrCreateRelease(Session& session, const MetaData::Album& album)
{
	Release::pointer release;

	// First try to get by MBID
	if (album.musicBrainzAlbumID)
	{
		release = Release::getByMBID(session, *album.musicBrainzAlbumID);
		if (!release)
		{
			release = Release::create(session, album.name, album.musicBrainzAlbumID);
		}
		else if (release->getName() != album.name)
		{
			// Name may have been updated
			release.modify()->setName(album.name);
		}

		return release;
	}

	// Fall back on release name (collisions may occur)
	if (!album.name.empty())
	{
		for (const Release::pointer& sameNamedRelease : Release::getByName(session, album.name))
		{
			// do not fallback on properly tagged releases
			if (!sameNamedRelease->getMBID())
			{
				release = sameNamedRelease;
				break;
			}
		}

		// No release found with the same name and without MBID -> creating
		if (!release)
			release = Release::create(session, album.name);

		return release;
	}

	return Release::pointer{};
}

std::vector<Cluster::pointer>
getOrCreateClusters(Session& session, const MetaData::Clusters& clustersNames)
{
	std::vector< Cluster::pointer > clusters;

	for (auto clusterNames : clustersNames)
	{
		auto clusterType = ClusterType::getByName(session, clusterNames.first);
		if (!clusterType)
			continue;

		for (auto clusterName : clusterNames.second)
		{
			auto cluster = clusterType->getCluster(clusterName);
			if (!cluster)
				cluster = Cluster::create(session, clusterType, clusterName);

			clusters.push_back(cluster);
		}
	}

	return clusters;
}

} // namespace

namespace Scanner {

std::unique_ptr<IMediaScanner>
createMediaScanner(Database::Db& db)
{
	return std::make_unique<MediaScanner>(db);
}

MediaScanner::MediaScanner(Database::Db& db)
: _dbSession {db}
{
	// For now, always use TagLib
	_metadataParser = std::make_unique<MetaData::TagLibParser>();

	_ioService.setThreadCount(1);

	refreshScanSettings();

	start();
}

MediaScanner::~MediaScanner()
{
	stop();
}

void
MediaScanner::start()
{
	std::scoped_lock lock {_controlMutex};

	scheduleNextScan();

	_ioService.start();
}

void
MediaScanner::stop()
{
	std::scoped_lock lock {_controlMutex};

	_abortScan = true;

	_scheduleTimer.cancel();
	_ioService.stop();
}

void
MediaScanner::abortScan()
{
	LMS_LOG(DBUPDATER, DEBUG) << "Aborting scan...";
	std::scoped_lock lock {_controlMutex};

	LMS_LOG(DBUPDATER, DEBUG) << "Waiting for the scan to abort...";

	_abortScan = true;
	_scheduleTimer.cancel();
	_ioService.stop();
	LMS_LOG(DBUPDATER, DEBUG) << "Scan abort done!";

	_abortScan = false;
	_ioService.start();
}

void
MediaScanner::requestImmediateScan(bool force)
{
	abortScan();
	_ioService.post([=]()
	{
		scheduleScan(force);
	});
}

void
MediaScanner::requestReload()
{
	abortScan();
	_ioService.post([=]()
	{
		scheduleNextScan();
	});
}

MediaScanner::Status
MediaScanner::getStatus() const
{
	Status res;

	std::shared_lock lock {_statusMutex};

	res.currentState = _curState;
	res.nextScheduledScan = _nextScheduledScan;
	res.lastCompleteScanStats = _lastCompleteScanStats;
	res.currentScanStepStats = _currentScanStepStats;

	return res;
}

void
MediaScanner::scheduleNextScan()
{
	LMS_LOG(DBUPDATER, INFO) << "Scheduling next scan";

	refreshScanSettings();

	const Wt::WDateTime now {Wt::WLocalDateTime::currentServerDateTime().toUTC()};

	Wt::WDate nextScanDate;
	switch (_updatePeriod)
	{
		case ScanSettings::UpdatePeriod::Daily:
			if (now.time() < _startTime)
				nextScanDate = now.date();
			else
				nextScanDate = now.date().addDays(1);
			break;

		case ScanSettings::UpdatePeriod::Weekly:
			if (now.time() < _startTime && now.date().dayOfWeek() == 1)
				nextScanDate = now.date();
			else
				nextScanDate = getNextMonday(now.date());
			break;

		case ScanSettings::UpdatePeriod::Monthly:
			if (now.time() < _startTime && now.date().day() == 1)
				nextScanDate = now.date();
			else
				nextScanDate = getNextFirstOfMonth(now.date());
			break;

		case ScanSettings::UpdatePeriod::Never:
			LMS_LOG(DBUPDATER, INFO) << "Auto scan disabled!";
			break;
	}

	Wt::WDateTime nextScanDateTime;

	if (nextScanDate.isValid())
	{
		nextScanDateTime = Wt::WDateTime {nextScanDate, _startTime};
		scheduleScan(false, nextScanDateTime);
	}

	{
		std::unique_lock lock {_statusMutex};
		_curState = nextScanDateTime.isValid() ? State::Scheduled : State::NotScheduled;
		_nextScheduledScan = nextScanDateTime;
	}

	_sigScheduled.emit(_nextScheduledScan);
}

void
MediaScanner::countAllFiles(ScanStats& stats)
{
	ScanStepStats stepStats{stats.startTime, ScanProgressStep::DiscoveringFiles};

	stats.filesScanned = 0;
	notifyInProgress(stepStats);

	exploreFilesRecursive(_mediaDirectory, [&](std::error_code ec, const std::filesystem::path& path)
	{
		if (_abortScan)
			return false;

		if (!ec && isFileSupported(path, _fileExtensions))
		{
			stats.filesScanned++;
			stepStats.processedFiles++;
			notifyInProgressIfNeeded(stepStats);
		}

		return true;
	});
}

void
MediaScanner::scheduleScan(bool force, const Wt::WDateTime& dateTime)
{
	auto cb {[=](boost::system::error_code ec)
	{
		if (ec)
			return;

		scan(force);
	}};

	if (dateTime.isNull())
	{
		LMS_LOG(DBUPDATER, INFO) << "Scheduling next scan right now";
		_scheduleTimer.expires_from_now(std::chrono::seconds {0});
		_scheduleTimer.async_wait(cb);
	}
	else
	{
		std::chrono::system_clock::time_point timePoint {dateTime.toTimePoint()};
		std::time_t t {std::chrono::system_clock::to_time_t(timePoint)};

		LMS_LOG(DBUPDATER, INFO) << "Scheduling next scan at " << std::string(std::ctime(&t));
		_scheduleTimer.expires_at(timePoint);
		_scheduleTimer.async_wait(cb);
	}
}

void
MediaScanner::scan(bool forceScan)
{
	scanStarted().emit();

	{
		std::unique_lock lock {_statusMutex};
		_curState = State::InProgress;
		_nextScheduledScan = {};
	}

	ScanStats stats;
	stats.startTime = Wt::WLocalDateTime::currentDateTime().toUTC();

	LMS_LOG(UI, INFO) << "New scan started!";

	refreshScanSettings();

	removeMissingTracks(stats);

	LMS_LOG(DBUPDATER, DEBUG) << "Counting files in media directory '" << _mediaDirectory.string() << "'...";
	countAllFiles(stats);
	LMS_LOG(DBUPDATER, DEBUG) << "-> Nb files = " << stats.filesScanned;

	LMS_LOG(UI, INFO) << "Checks complete, force scan = " << forceScan;

	LMS_LOG(DBUPDATER, INFO) << "scaning media directory '" << _mediaDirectory.string() << "'...";
	scanMediaDirectory(_mediaDirectory, forceScan, stats);
	LMS_LOG(DBUPDATER, INFO) << "scaning media directory '" << _mediaDirectory.string() << "' DONE";

	removeOrphanEntries();

	if (!_abortScan)
		checkDuplicatedAudioFiles(stats);

	// Now update all the track features if needed
	fetchTrackFeatures(stats);

	LMS_LOG(DBUPDATER, INFO) << "Scan " << (_abortScan ? "aborted" : "complete") << ". Changes = " << stats.nbChanges() << " (added = " << stats.additions << ", removed = " << stats.deletions << ", updated = " << stats.updates << "), Not changed = " << stats.skips << ", Scanned = " << stats.scans << " (errors = " << stats.errors.size() << "), features fetched = " << stats.featuresFetched << ",  duplicates = " << stats.duplicates.size();

	_dbSession.optimize();

	if (!_abortScan)
	{
		stats.stopTime = Wt::WLocalDateTime::currentDateTime().toUTC();
		{
			std::unique_lock lock {_statusMutex};

			_lastCompleteScanStats = std::move(stats);
			_currentScanStepStats.reset();
		}

		LMS_LOG(DBUPDATER, DEBUG) << "Scan not aborted, scheduling next scan!";
		scheduleNextScan();

		scanComplete().emit();
	}
	else
	{
		LMS_LOG(DBUPDATER, DEBUG) << "Scan aborted, not scheduling next scan!";

		std::unique_lock lock {_statusMutex};

		_curState = State::NotScheduled;
		_currentScanStepStats.reset();
	}
}

bool
MediaScanner::fetchTrackFeatures(Database::IdType trackId, const UUID& MBID)
{
	std::map<std::string, double> features;

	LMS_LOG(DBUPDATER, INFO) << "Fetching low level features for track '" << MBID.getAsString() << "'";
	const std::string data {AcousticBrainz::extractLowLevelFeatures(MBID)};
	if (data.empty())
	{
		LMS_LOG(DBUPDATER, ERROR) << "Track " << trackId << ", MBID = '" << MBID.getAsString() << "': cannot extract features using AcousticBrainz";
		return false;
	}

	{
		auto uniqueTransaction {_dbSession.createUniqueTransaction()};

		Wt::Dbo::ptr<Database::Track> track {Database::Track::getById(_dbSession, trackId)};
		if (!track)
			return false;

		Database::TrackFeatures::create(_dbSession, track, data);
	}

	return true;
}

void
MediaScanner::fetchTrackFeatures(ScanStats& stats)
{
	if (_recommendationEngineType != ScanSettings::RecommendationEngineType::Features)
		return;

	ScanStepStats stepStats{stats.startTime, ScanProgressStep::FetchingTrackFeatures};

	LMS_LOG(DBUPDATER, INFO) << "Fetching missing track features...";

	struct TrackInfo
	{
		Database::IdType id;
		UUID mbid;
	};

	const auto tracksToFetch {[&]()
	{
		std::vector<TrackInfo> res;

		auto transaction {_dbSession.createSharedTransaction()};

		auto tracks {Database::Track::getAllWithMBIDAndMissingFeatures(_dbSession)};
		for (const auto& track : tracks)
			res.emplace_back(TrackInfo {track.id(), *track->getMBID()});

		return res;
	}()};

	stepStats.filesToProcess = tracksToFetch.size();
	notifyInProgress(stepStats);

	LMS_LOG(DBUPDATER, INFO) << "Found " << tracksToFetch.size() << " track(s) to fetch!";

	for (const TrackInfo& trackToFetch : tracksToFetch)
	{
		if (_abortScan)
			return;

		if (fetchTrackFeatures(trackToFetch.id, trackToFetch.mbid))
			stats.featuresFetched++;

		stepStats.processedFiles++;
		notifyInProgressIfNeeded(stepStats);
	}

	LMS_LOG(DBUPDATER, INFO) << "Track features fetched!";
}

void
MediaScanner::refreshScanSettings()
{
	auto transaction {_dbSession.createSharedTransaction()};

	ScanSettings::pointer scanSettings {ScanSettings::get(_dbSession)};

	LMS_LOG(DBUPDATER, INFO) << "Using scan settings version " << scanSettings->getScanVersion();

	_scanVersion = scanSettings->getScanVersion();
	_startTime = scanSettings->getUpdateStartTime();
	_updatePeriod = scanSettings->getUpdatePeriod();

	{
		const auto fileExtensions {scanSettings->getAudioFileExtensions()};
		_fileExtensions.clear();
		std::transform(std::cbegin(fileExtensions), std::end(fileExtensions), std::inserter(_fileExtensions, std::begin(_fileExtensions)),
				[](const std::filesystem::path& extension) { return std::filesystem::path{ StringUtils::stringToLower(extension.string()) }; });
	}
	_mediaDirectory = scanSettings->getMediaDirectory();
	_recommendationEngineType = scanSettings->getRecommendationEngineType();

	auto clusterTypes = scanSettings->getClusterTypes();
	std::set<std::string> clusterTypeNames;

	std::transform(std::cbegin(clusterTypes), std::cend(clusterTypes),
			std::inserter(clusterTypeNames, clusterTypeNames.begin()),
			[](ClusterType::pointer clusterType) { return clusterType->getName(); });

	_metadataParser->setClusterTypeNames(clusterTypeNames);

}

void
MediaScanner::notifyInProgress(const ScanStepStats& stepStats)
{
	{
		std::unique_lock lock {_statusMutex};
		_currentScanStepStats = stepStats;
	}

	const std::chrono::system_clock::time_point now {std::chrono::system_clock::now()};
	_sigScanInProgress(stepStats);
	_lastScanInProgressEmit = now;
}

void
MediaScanner::notifyInProgressIfNeeded(const ScanStepStats& stepStats)
{
	std::chrono::system_clock::time_point now {std::chrono::system_clock::now()};

	if (std::chrono::duration_cast<std::chrono::seconds>(now - _lastScanInProgressEmit).count() > 1)
		notifyInProgress(stepStats);
}

void
MediaScanner::scanAudioFile(const std::filesystem::path& file, bool forceScan, ScanStats& stats)
{
	Wt::WDateTime lastWriteTime;
	try
	{
		lastWriteTime = getLastWriteTime(file);
	}
	catch (LmsException& e)
	{
		LMS_LOG(DBUPDATER, ERROR) << e.what();
		stats.skips++;
		return;
	}

	if (!forceScan)
	{
		// Skip file if last write is the same
		auto transaction {_dbSession.createSharedTransaction()};

		const Track::pointer track {Track::getByPath(_dbSession, file)};

		if (track && track->getLastWriteTime().toTime_t() == lastWriteTime.toTime_t()
				&& track->getScanVersion() == _scanVersion)
		{
			stats.skips++;
			return;
		}
	}

	std::optional<MetaData::Track> trackInfo {_metadataParser->parse(file)};
	if (!trackInfo)
	{
		stats.errors.emplace_back(file, ScanErrorType::CannotParseFile);
		return;
	}

	stats.scans++;

	auto uniqueTransaction {_dbSession.createUniqueTransaction()};

	Track::pointer track {Track::getByPath(_dbSession, file) };

	// We estimate this is an audio file if:
	// - we found a least one audio stream
	// - the duration is not null
	if (trackInfo->audioStreams.empty())
	{
		LMS_LOG(DBUPDATER, INFO) << "Skipped '" << file.string() << "' (no audio stream found)";

		// If Track exists here, delete it!
		if (track)
		{
			track.remove();
			stats.deletions++;
		}
		stats.errors.emplace_back(ScanError {file, ScanErrorType::NoAudioTrack});
		return;
	}
	if (trackInfo->duration == std::chrono::milliseconds::zero())
	{
		LMS_LOG(DBUPDATER, INFO) << "Skipped '" << file.string() << "' (duration is 0)";

		// If Track exists here, delete it!
		if (track)
		{
			track.remove();
			stats.deletions++;
		}
		stats.errors.emplace_back(ScanError {file, ScanErrorType::BadDuration});
		return;
	}

	// ***** Title
	std::string title;
	if (!trackInfo->title.empty())
		title = trackInfo->title;
	else
	{
		// TODO parse file name guess track etc.
		// For now juste use file name as title
		title = file.filename().string();
	}

	// ***** Clusters
	std::vector<Cluster::pointer> clusters {getOrCreateClusters(_dbSession, trackInfo->clusters)};

	//  ***** Artists
	std::vector<Artist::pointer> artists {getOrCreateArtists(_dbSession, trackInfo->artists)};

	//  ***** Release artists
	std::vector<Artist::pointer> releaseArtists {getOrCreateArtists(_dbSession, trackInfo->albumArtists)};

	//  ***** Release
	Release::pointer release;
	if (trackInfo->album)
		release = getOrCreateRelease(_dbSession, *trackInfo->album);

	// If file already exist, update data
	// Otherwise, create it
	if (!track)
	{
		// Create a new song
		track = Track::create(_dbSession, file);
		LMS_LOG(DBUPDATER, INFO) << "Adding '" << file.string() << "'";
		stats.additions++;
	}
	else
	{
		LMS_LOG(DBUPDATER, INFO) << "Updating '" << file.string() << "'";

		stats.updates++;
	}

	// Track related data
	assert(track);

	track.modify()->clearArtistLinks();
	for (const auto& artist : artists)
		track.modify()->addArtistLink(Database::TrackArtistLink::create(_dbSession, track, artist, Database::TrackArtistLink::Type::Artist));

	for (const auto& releaseArtist : releaseArtists)
		track.modify()->addArtistLink(Database::TrackArtistLink::create(_dbSession, track, releaseArtist, Database::TrackArtistLink::Type::ReleaseArtist));

	track.modify()->setScanVersion(_scanVersion);
	track.modify()->setRelease(release);
	track.modify()->setClusters(clusters);
	track.modify()->setLastWriteTime(lastWriteTime);
	track.modify()->setName(title);
	track.modify()->setDuration(trackInfo->duration);
	track.modify()->setAddedTime(Wt::WLocalDateTime::currentServerDateTime().toUTC());
	track.modify()->setTrackNumber(trackInfo->trackNumber ? *trackInfo->trackNumber : 0);
	track.modify()->setDiscNumber(trackInfo->discNumber ? *trackInfo->discNumber : 0);
	track.modify()->setTotalTrack(trackInfo->totalTrack);
	track.modify()->setTotalDisc(trackInfo->totalDisc);
	if (!trackInfo->discSubtitle.empty())
		track.modify()->setDiscSubtitle(trackInfo->discSubtitle);
	track.modify()->setYear(trackInfo->year ? *trackInfo->year : 0);
	track.modify()->setOriginalYear(trackInfo->originalYear ? *trackInfo->originalYear : 0);

	// If a file has an OriginalYear but no Year, set it to ease filtering
	if (!trackInfo->year && trackInfo->originalYear)
		track.modify()->setYear(*trackInfo->originalYear);

	track.modify()->setMBID(trackInfo->musicBrainzRecordID);
	track.modify()->setFeatures({}); // TODO: only if MBID changed?
	track.modify()->setHasCover(trackInfo->hasCover);
	track.modify()->setCopyright(trackInfo->copyright);
	track.modify()->setCopyrightURL(trackInfo->copyrightURL);
	if (trackInfo->trackReplayGain)
		track.modify()->setTrackReplayGain(*trackInfo->trackReplayGain);
	if (trackInfo->albumReplayGain)
		track.modify()->setReleaseReplayGain(*trackInfo->albumReplayGain);
}

void
MediaScanner::scanMediaDirectory(const std::filesystem::path& mediaDirectory, bool forceScan, ScanStats& stats)
{
	ScanStepStats stepStats{stats.startTime, ScanProgressStep::ScanningFiles};
	stepStats.filesToProcess = stats.filesScanned;
	notifyInProgress(stepStats);

	exploreFilesRecursive(mediaDirectory, [&](std::error_code ec, const std::filesystem::path& path)
	{
		if (_abortScan)
			return false;

		if (ec)
		{
			LMS_LOG(DBUPDATER, ERROR) << "Cannot process entry '" << path.string() << "': " << ec.message();
			stats.errors.emplace_back(ScanError {path, ScanErrorType::CannotReadFile, ec.message()});
		}
		else if (isFileSupported(path, _fileExtensions))
		{
			scanAudioFile(path, forceScan, stats );

			stepStats.processedFiles++;
			notifyInProgressIfNeeded(stepStats);
		}

		return true;
	});
}

// Check if a file exists and is still in a media directory
static bool
checkFile(const std::filesystem::path& p, const std::filesystem::path& mediaDirectory, const std::unordered_set<std::filesystem::path>& extensions)
{
	try
	{
		// For each track, make sure the the file still exists
		// and still belongs to a media directory
		if (!std::filesystem::exists( p )
			|| !std::filesystem::is_regular_file( p ) )
		{
			LMS_LOG(DBUPDATER, INFO) << "Removing '" << p.string() << "': missing";
			return false;
		}

		if (!isPathInParentPath(p, mediaDirectory))
		{
			LMS_LOG(DBUPDATER, INFO) << "Removing '" << p.string() << "': out of media directory";
			return false;
		}

		if (!isFileSupported(p, extensions))
		{
			LMS_LOG(DBUPDATER, INFO) << "Removing '" << p.string() << "': file format no longer handled";
			return false;
		}

		return true;

	}
	catch (std::filesystem::filesystem_error& e)
	{
		LMS_LOG(DBUPDATER, ERROR) << "Caught exception while checking file '" << p.string() << "': " << e.what();
		return false;
	}
}

void
MediaScanner::removeMissingTracks(ScanStats& stats)
{
	static constexpr std::size_t batchSize {50};

	ScanStepStats stepStats{stats.startTime, ScanProgressStep::ChekingForMissingFiles};

	LMS_LOG(DBUPDATER, DEBUG) << "Checking tracks to be removed...";
	std::size_t trackCount {};

	{
		auto transaction {_dbSession.createSharedTransaction()};
		trackCount = Track::getCount(_dbSession);
	}
	LMS_LOG(DBUPDATER, DEBUG) << trackCount << " tracks to be checked...";

	stepStats.filesToProcess = trackCount;
	notifyInProgress(stepStats);

	std::vector<std::pair<Database::IdType, std::filesystem::path>> trackPaths;
	std::vector<IdType> tracksToRemove;

	for (std::size_t i {trackCount < batchSize ? 0 : trackCount - batchSize}; ; i -= (i > batchSize ? batchSize : i))
	{
		trackPaths.clear();
		tracksToRemove.clear();

		{
			auto transaction {_dbSession.createSharedTransaction()};
			trackPaths = Track::getAllPaths(_dbSession, i, batchSize);
		}

		for (const auto& [trackId, trackPath] : trackPaths)
		{
			if (_abortScan)
				return;

			if (!checkFile(trackPath, _mediaDirectory, _fileExtensions))
				tracksToRemove.push_back(trackId);

			stepStats.processedFiles++;
		}

		if (!tracksToRemove.empty())
		{
			auto transaction {_dbSession.createUniqueTransaction()};

			for (const IdType trackId : tracksToRemove)
			{
				Track::pointer track {Track::getById(_dbSession, trackId)};
				if (track)
				{
					track.remove();
					stats.deletions++;
				}
			}
		}

		notifyInProgressIfNeeded(stepStats);

		if (i == 0)
			break;
	}

	LMS_LOG(DBUPDATER, DEBUG) << trackCount << " tracks checked!";
}

void
MediaScanner::removeOrphanEntries()
{
	LMS_LOG(DBUPDATER, DEBUG) << "Checking orphan clusters...";
	{
		auto transaction {_dbSession.createUniqueTransaction()};

		// Now process orphan Cluster (no track)
		auto clusters {Cluster::getAllOrphans(_dbSession)};
		for (auto& cluster : clusters)
		{
			LMS_LOG(DBUPDATER, DEBUG) << "Removing orphan cluster '" << cluster->getName() << "'";
			cluster.remove();
		}
	}

	LMS_LOG(DBUPDATER, DEBUG) << "Checking orphan artists...";
	{
		auto transaction {_dbSession.createUniqueTransaction()};

		auto artists {Artist::getAllOrphans(_dbSession)};
		for (auto& artist : artists)
		{
			LMS_LOG(DBUPDATER, DEBUG) << "Removing orphan artist '" << artist->getName() << "'";
			artist.remove();
		}
	}

	LMS_LOG(DBUPDATER, DEBUG) << "Checking orphan releases...";
	{
		auto transaction {_dbSession.createUniqueTransaction()};

		auto releases {Release::getAllOrphans(_dbSession)};
		for (auto& release : releases)
		{
			LMS_LOG(DBUPDATER, DEBUG) << "Removing orphan release '" << release->getName() << "'";
			release.remove();
		}
	}

	LMS_LOG(DBUPDATER, INFO) << "Check audio files done!";
}

void
MediaScanner::checkDuplicatedAudioFiles(ScanStats& stats)
{
	LMS_LOG(DBUPDATER, INFO) << "Checking duplicated audio files";

	auto transaction {_dbSession.createSharedTransaction()};

	const std::vector<Track::pointer> tracks = Database::Track::getMBIDDuplicates(_dbSession);
	for (const Track::pointer& track : tracks)
	{
		if (track->getMBID())
		{
			LMS_LOG(DBUPDATER, INFO) << "Found duplicated MBID [" << track->getMBID()->getAsString() << "], file: " << track->getPath().string() << " - " << track->getName();
			stats.duplicates.emplace_back(ScanDuplicate {track->getPath(), DuplicateReason::SameMBID});
		}
	}

	LMS_LOG(DBUPDATER, INFO) << "Checking duplicated audio files done!";
}

} // namespace Scanner
