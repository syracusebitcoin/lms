/*
 * Copyright (C) 2018 Emeric Poupon
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

#pragma once

#include <map>
#include <optional>
#include <set>
#include <string>

#include "database/Types.hpp"
#include "som/DataNormalizer.hpp"
#include "som/Network.hpp"
#include "SimilarityFeaturesCache.hpp"
#include "SimilarityFeaturesDefs.hpp"

namespace Database
{
	class Session;
}

namespace Similarity {

using FeatureWeight = double;

class FeaturesSearcher
{
	public:

		using StopRequestedFunction = std::function<bool()>; // return true if stop requested

		// Use cache
		FeaturesSearcher(Database::Session& session, FeaturesCache cache, StopRequestedFunction stopRequested);

		// Use training (may be very slow)
		struct TrainSettings
		{
			std::size_t iterationCount {10};
			float sampleCountPerNeuron {4};
			FeatureSettingsMap featureSettingsMap;
		};
		FeaturesSearcher(Database::Session& session, const TrainSettings& trainSettings, StopRequestedFunction stopRequested = {});

		static const FeatureSettingsMap& getDefaultTrainFeatureSettings();

		bool isValid() const;

		bool isTrackClassified(Database::IdType trackId) const;
		bool isReleaseClassified(Database::IdType releaseId) const;
		bool isArtistClassified(Database::IdType artistId) const;

		std::vector<Database::IdType> getSimilarTracks(const std::set<Database::IdType>& tracksId, std::size_t maxCount) const;
		std::vector<Database::IdType> getSimilarReleases(Database::IdType releaseId, std::size_t maxCount) const;
		std::vector<Database::IdType> getSimilarArtists(Database::IdType artistId, std::size_t maxCount) const;

		void dump(Database::Session& session, std::ostream& os) const;

		FeaturesCache toCache() const;

		using FeaturesFetchFunc = std::function<std::optional<std::unordered_map<std::string, std::vector<double>>>(Database::IdType /*trackId*/, const std::unordered_set<std::string>& /*features*/)>;
		// Default is to retrieve the features from the database (may be slow).
		// Use this only if you want to train different searchers with the same data
		static void setFeaturesFetchFunc(FeaturesFetchFunc func) { _featuresFetchFunc = func; }

	private:

		using ObjectPositions = std::map<Database::IdType, std::set<SOM::Position>>;

		void init(Database::Session& session,
				SOM::Network network,
				ObjectPositions tracksPosition,
				StopRequestedFunction stopRequested);

		std::vector<Database::IdType> getSimilarObjects(const std::set<Database::IdType>& ids,
				const SOM::Matrix<std::set<Database::IdType>>& objectsMap,
				const ObjectPositions& objectPosition,
				std::size_t maxCount) const;

		std::unique_ptr<SOM::Network>	_network;
		double				_networkRefVectorsDistanceMedian {};

		SOM::Matrix<std::set<Database::IdType>> 	_artistsMap;
		ObjectPositions					_artistPositions;

		SOM::Matrix<std::set<Database::IdType>> 	_releasesMap;
		ObjectPositions					_releasePositions;

		SOM::Matrix<std::set<Database::IdType>> 	_tracksMap;
		ObjectPositions					_trackPositions;

		static inline FeaturesFetchFunc _featuresFetchFunc;
};

} // ns Similarity