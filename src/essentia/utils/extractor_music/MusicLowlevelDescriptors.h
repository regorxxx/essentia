/*
 * Copyright (C) 2006-2021  Music Technology Group - Universitat Pompeu Fabra
 *
 * This file is part of Essentia
 *
 * Essentia is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the Free
 * Software Foundation (FSF), either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the Affero GNU General Public License
 * version 3 along with this program.  If not, see http://www.gnu.org/licenses/
 */

#ifndef MUSIC_LOWLEVEL_DESCRIPTORS_H
#define MUSIC_LOWLEVEL_DESCRIPTORS_H

#include "MusicDescriptorsSet.h"
#include "essentia/essentiamath.h"

class MusicLowlevelDescriptors : public MusicDescriptorSet {

 public:
 	static const std::string nameSpace;

  MusicLowlevelDescriptors(Pool& options) {
    this->options = options;
  }
  ~MusicLowlevelDescriptors();

 	void createNetworkNeqLoud(SourceBase& source, Pool& pool);
  void createNetworkEqLoud(SourceBase& source, Pool& pool);
  void createNetworkLoudness(SourceBase& source, Pool& pool);
	void computeAverageLoudness(Pool& pool);
};

#endif