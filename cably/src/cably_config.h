/*
 * This program source code file is part of Cably Desktop, based on KiCad,
 * a free EDA CAD application.
 *
 * Copyright (C) 2026 Cably <dev@cably.dev>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file cably_config.h
 * Product identity constants for Cably Desktop (F2).
 *
 * Everything user-visible that names the product goes through these, so the
 * name is defined once. KiCad attribution ("based on KiCad") is REQUIRED by the
 * KiCad trademark policy and by GPLv3 s5/s6 and must not be removed. The
 * product is never called "Cably KiCad".
 *
 * No secrets, no cloud endpoints: this header ships in the published source.
 */

#ifndef CABLY_CONFIG_H
#define CABLY_CONFIG_H

/// The product name as shown in window titles, the About dialog and bundle metadata.
#define CABLY_PRODUCT_NAME "Cably Desktop"

/// Mandatory attribution phrase; always shown next to the product name.
#define CABLY_BASED_ON "based on KiCad"

/// Public website.
#define CABLY_WEBSITE_URL "https://cably.dev"

/// Where the Complete Corresponding Source of THIS build is published (GPLv3 s6).
/// PLACEHOLDER until F7 (publishing): F7 must replace it with the real, reachable
/// repository/tag URL before any binary is distributed.
#define CABLY_SOURCE_URL "https://cably.dev/desktop/source"

/// Bug reports for Cably Desktop go here, never to KiCad's tracker or forum.
/// PLACEHOLDER until F7.
#define CABLY_BUGS_URL "https://cably.dev/desktop/issues"

#endif // CABLY_CONFIG_H
