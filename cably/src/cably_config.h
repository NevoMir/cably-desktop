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
 * product name never combines "Cably" with the KiCad mark.
 *
 * No secrets: this header ships in the published source. The cloud endpoints below
 * are public, and the Supabase "publishable" key is public by design (it is embedded in
 * every cably.dev web bundle; access control is the user's bearer token + row-level
 * security, never this key).
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

/// Public cloud endpoints and the public Supabase key used by the F4 cloud bridge
/// (cably/src/cably_bridge.h). Row-level security + the user's bearer token gate every
/// call; the publishable key only identifies the project.
#define CABLY_SUPABASE_URL "https://bhuzwogxxeyolpadhisl.supabase.co"
#define CABLY_SUPABASE_PUBLISHABLE_KEY "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImJodXp3b2d4eGV5b2xwYWRoaXNsIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NzE0MTg3NjksImV4cCI6MjA4Njk5NDc2OX0.mSmikj6NDiog1duZ8_eAr4CECLarlREXYN_y6TWcaEE"
#define CABLY_ENGINE_URL "https://engine.cably.dev"

/// The web page that hands a signed-in session to the desktop (F4 loopback handoff).
#define CABLY_DESKTOP_AUTH_URL "https://cably.dev/desktop/auth"

/// The web app; "Generate" opens it with ?prompt=<urlencoded> (generation stays on the web).
#define CABLY_APP_URL "https://cably.dev/app"

/// macOS keychain service name under which the desktop session is stored.
#define CABLY_KEYCHAIN_SERVICE "dev.cably.desktop"

#endif // CABLY_CONFIG_H
