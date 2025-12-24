/* Test program for fetching DWO files via debuginfod.
   Copyright (C) 2025 Red Hat, Inc.
   This file is part of elfutils.

   This file is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   elfutils is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */


#ifdef HAVE_CONFIG_H
# include <config.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include ELFUTILS_HEADER(dwfl)
#include <elf.h>
#include <dwarf.h>

/* Test that libdwfl can find split DWARF via debuginfod.
   Usage: debuginfod_dwoid_find SKELETON_FILE

   The SKELETON_FILE should be a binary with split DWARF that references
   a DWO/DWP file. The DWO/DWP should NOT be in the local directory,
   but should be available via the debuginfod server in DEBUGINFOD_URLS.

   This test verifies that the DWO lookup callback is invoked and
   successfully retrieves the split DWARF data.  */

static const char *debuginfo_path = "";
static const Dwfl_Callbacks cb =
  {
    NULL,
    dwfl_standard_find_debuginfo,
    NULL,
    (char **)&debuginfo_path,
  };

int
main (int argc, char **argv)
{
  if (argc < 2)
    {
      fprintf (stderr, "Usage: %s SKELETON_FILE [expect_split]\n", argv[0]);
      return 1;
    }

  int expect_split = 1;  /* By default, expect to find split units */
  if (argc >= 3)
    expect_split = atoi (argv[2]);

  Dwfl *dwfl = dwfl_begin (&cb);
  if (dwfl == NULL)
    {
      fprintf (stderr, "dwfl_begin failed: %s\n", dwfl_errmsg (-1));
      return 1;
    }

  dwfl_report_begin (dwfl);

  /* Open the skeleton file.  */
  Dwfl_Module *mod = dwfl_report_offline (dwfl, argv[1], argv[1], -1);
  if (mod == NULL)
    {
      fprintf (stderr, "dwfl_report_offline failed: %s\n", dwfl_errmsg (-1));
      dwfl_end (dwfl);
      return 1;
    }

  dwfl_report_end (dwfl, NULL, NULL);

  /* Get the DWARF info - this should trigger DWO lookup if needed.  */
  Dwarf_Addr bias = 0;
  Dwarf *dbg = dwfl_module_getdwarf (mod, &bias);
  if (dbg == NULL)
    {
      fprintf (stderr, "dwfl_module_getdwarf failed: %s\n", dwfl_errmsg (-1));
      dwfl_end (dwfl);
      return 1;
    }

  /* Iterate over compilation units to find skeleton/split units.  */
  int found_split = 0;
  Dwarf_CU *cu = NULL;
  Dwarf_Die cudie, subdie;
  uint8_t unit_type;

  while (dwarf_get_units (dbg, cu, &cu, NULL, &unit_type, &cudie, &subdie) == 0)
    {
      const char *name = dwarf_diename (&cudie);
      printf ("Found CU: %s (unit_type=%d)\n", name ? name : "(null)", unit_type);

      if (unit_type == DW_UT_skeleton)
        {
          printf ("  This is a skeleton unit\n");

          /* The subdie contains the split DIE if found.  */
          if (dwarf_tag (&subdie) != 0)
            {
              const char *split_name = dwarf_diename (&subdie);
              printf ("  Found split DIE: %s\n", split_name ? split_name : "(null)");
              found_split = 1;
            }
          else
            {
              printf ("  Could not find split DIE\n");
            }
        }
    }

  dwfl_end (dwfl);

  if (expect_split && !found_split)
    {
      fprintf (stderr, "Expected to find split DWARF but did not\n");
      return 1;
    }

  if (!expect_split && found_split)
    {
      fprintf (stderr, "Did not expect to find split DWARF but did\n");
      return 1;
    }

  printf ("Test passed: found_split=%d, expect_split=%d\n", found_split, expect_split);
  return 0;
}
