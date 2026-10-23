#pragma once

#include <vector>
#include "ADM_Video.h"

// Reads QuickTime chapter tracks referenced by a video track's tref/chap atom.
bool ADM_mp4ReadChapterMarkers(const char *path, std::vector<ADM_ChapterMarker> &chapters);
