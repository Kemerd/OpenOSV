/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 The OpenOSV Contributors
 *
 * OpenOSVImporter.r - the macOS resource of the importer bundle.
 *
 * Premiere recognises a standard importer on a Mac by an 'IMPT' resource in
 * the bundle, exactly as it does by the IMPT entry of OpenOSVImporter.rc on
 * Windows (and as Adobe's SDK_File_ImportPiPL.r declares it).  The value is
 * the importer's file type four-character code, 'OSV_' - kOsvFileType in
 * ImporterPlugin.h and OSV_FILETYPE in the .rc - which After Effects also
 * uses as the importer's draw type.
 *
 * Compiled by Rez into Contents/Resources/OpenOSVImporter.rsrc
 * (osv_add_pipl() in cmake/OsvPremiereSdk.cmake).
 */

type 'IMPT'
{
	longint;
};

resource 'IMPT' (1000)
{
	0x4F53565F	/* 'OSV_' */
};
