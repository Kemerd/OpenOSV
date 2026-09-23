// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// paths.js - media paths the OSV rule must accept and must refuse.
//
// Shared by osvcore.test.js and hostjsx.test.js, so the panel's rule
// (OsvCore.isOsvMediaPath) and the ExtendScript copy of it in host.jsx are
// held to one corpus and cannot drift apart.
'use strict';

const OSV_PATHS = [
    'C:\\Footage\\CAM_20260101_0001.OSV',
    'C:/Footage/clip.osv',
    'D:\\proxy\\CAM_0001.LRF',
    '/Volumes/SSD/DCIM/CAM_0001.lrf',
    '\\\\nas\\share\\trip\\A.Osv',
    '  C:\\spaces\\clip.OSV  ',
    'C:\\trailing\\nul.osv\u0000\u0000',
    'relative.osv',
    'C:\\dots.in.name\\clip.v2.OSV'
];

const NOT_OSV_PATHS = [
    '',
    '   ',
    'C:\\Footage\\clip.mp4',
    'C:\\Footage\\clip.osv.mp4',
    'C:\\trip.osv\\clip.mp4',
    'C:\\trip.osv\\',
    'C:/trip.osv/',
    'C:\\Footage\\.osv',
    'C:\\Footage\\clip.',
    'C:\\Footage\\cliposv',
    'C:\\Footage\\clip.osvx',
    'C:\\Footage\\clip.lrfx',
    'C:\\Footage\\clip.osv ext',
    'osv',
    '.lrf'
];

module.exports = { OSV_PATHS, NOT_OSV_PATHS };
