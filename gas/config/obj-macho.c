/* Mach-O object file format
   Copyright 2009, 2011, 2012 Free Software Foundation, Inc.

   This file is part of GAS, the GNU Assembler.

   GAS is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 3,
   or (at your option) any later version.

   GAS is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See
   the GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with GAS; see the file COPYING.  If not, write to the Free
   Software Foundation, 51 Franklin Street - Fifth Floor, Boston, MA
   02110-1301, USA.  */

/* Here we handle the mach-o directives that are common to all architectures.

   Most significant are mach-o named sections and a variety of symbol type
   decorations.  */

/* Mach-O supports multiple, named segments each of which may contain
   multiple named sections.  Thus the concept of subsectioning is 
   handled by (say) having a __TEXT segment with appropriate flags from
   which subsections are generated like __text, __const etc.  
   
   The well-known as short-hand section switch directives like .text, .data
   etc. are mapped onto predefined segment/section pairs using facilites
   supplied by the mach-o port of bfd.
   
   A number of additional mach-o short-hand section switch directives are
   also defined.  */

#define OBJ_HEADER "obj-macho.h"

#include "as.h"
#include "subsegs.h"
#include "symbols.h"
#include "write.h"
#include "mach-o.h"
#include "mach-o/loader.h"
#include "obj-macho.h"

/* Forward decls.  */
static segT obj_mach_o_segT_from_bfd_name (const char *, int);

/* TODO: Implement "-dynamic"/"-static" command line options.  */

static int obj_mach_o_is_static;

/* TODO: Implement the "-n" command line option to suppress the initial
   switch to the text segment.  */
static int obj_mach_o_start_with_text_section = 1;

/* Allow for special re-ordering on output.  */

static int obj_mach_o_seen_objc_section;

/* Start-up: At present, just create the sections we want.  */
void
mach_o_begin (void)
{
  /* Mach-O only defines the .text section by default, and even this can
     be suppressed by a flag.  In the latter event, the first code MUST
     be a section definition.  */
  if (obj_mach_o_start_with_text_section)
    {
      text_section = obj_mach_o_segT_from_bfd_name (TEXT_SECTION_NAME, 1);
      subseg_set (text_section, 0);
      if (obj_mach_o_is_static)
	{
	  bfd_mach_o_section *mo_sec 
			= bfd_mach_o_get_mach_o_section (text_section);
	  mo_sec->flags &= ~BFD_MACH_O_S_ATTR_PURE_INSTRUCTIONS;
	}
    }
}

/* Remember the subsections_by_symbols state in case we need to reset
   the file flags.  */
static int obj_mach_o_subsections_by_symbols;

static void
obj_mach_o_weak (int ignore ATTRIBUTE_UNUSED)
{
  char *name;
  int c;
  symbolS *symbolP;

  do
    {
      /* Get symbol name.  */
      name = input_line_pointer;
      c = get_symbol_end ();
      symbolP = symbol_find_or_make (name);
      S_SET_WEAK (symbolP);
      *input_line_pointer = c;
      SKIP_WHITESPACE ();

      if (c != ',')
        break;
      input_line_pointer++;
      SKIP_WHITESPACE ();
    }
  while (*input_line_pointer != '\n');
  demand_empty_rest_of_line ();
}

/* This will put at most 16 characters (terminated by a ',' or newline) from
   the input stream into dest.  If there are more than 16 chars before the
   delimiter, a warning is given and the string is truncated.  On completion of
   this function, input_line_pointer will point to the char after the ',' or 
   to the newline.  
   
   It trims leading and trailing space.  */

static int
collect_16char_name (char *dest, const char *msg, int require_comma)
{
  char c, *namstart;

  SKIP_WHITESPACE ();
  namstart = input_line_pointer;

  while ( (c = *input_line_pointer) != ',' 
	 && !is_end_of_line[(unsigned char) c])
    input_line_pointer++;

  {
      int len = input_line_pointer - namstart; /* could be zero.  */
      /* lose any trailing space.  */  
      while (len > 0 && namstart[len-1] == ' ') 
        len--;
      if (len > 16)
        {
          *input_line_pointer = '\0'; /* make a temp string.  */
	  as_bad (_("the %s name '%s' is too long (maximum 16 characters)"),
		     msg, namstart);
	  *input_line_pointer = c; /* restore for printing.  */
	  len = 16;
	}
      if (len > 0)
        memcpy (dest, namstart, len);
  }

  if (c != ',' && require_comma)
    {
      as_bad (_("expected a %s name followed by a `,'"), msg);
      return 1;
    }

  return 0;
}

static int
obj_mach_o_get_section_names (char *seg, char *sec,
			      unsigned segl, unsigned secl)
{
  /* Zero-length segment and section names are allowed.  */
  /* Parse segment name.  */
  memset (seg, 0, segl);
  if (collect_16char_name (seg, "segment", 1))
    {
      ignore_rest_of_line ();
      return 0;
    }
  input_line_pointer++; /* Skip the terminating ',' */

  /* Parse section name, which can be empty.  */
  memset (sec, 0, secl);
  collect_16char_name (sec, "section", 0);
  return 1;
}

/* Build (or get) a section from the mach-o description - which includes
   optional definitions for type, attributes, alignment and stub size.
   
   BFD supplies default values for sections which have a canonical name.  */

#define SECT_TYPE_SPECIFIED 0x0001
#define SECT_ATTR_SPECIFIED 0x0002
#define SECT_ALGN_SPECIFIED 0x0004

static segT
obj_mach_o_make_or_get_sect (char * segname, char * sectname,
			     unsigned int specified_mask, 
			     unsigned int usectype, unsigned int usecattr,
			     unsigned int ualign, offsetT stub_size)
{
  unsigned int sectype, secattr, secalign;
  flagword oldflags, flags;
  const char *name;
  segT sec;
  bfd_mach_o_section *msect;
  const mach_o_section_name_xlat *xlat;

  /* This provides default bfd flags and default mach-o section type and
     attributes along with the canonical name.  */
  xlat = bfd_mach_o_section_data_for_mach_sect (stdoutput, segname, sectname);

  /* TODO: more checking of whether overides are acually allowed.  */

  if (xlat != NULL)
    {
      name = xstrdup (xlat->bfd_name);
      sectype = xlat->macho_sectype;
      if (specified_mask & SECT_TYPE_SPECIFIED)
	{
	  if ((sectype == BFD_MACH_O_S_ZEROFILL
	       || sectype == BFD_MACH_O_S_GB_ZEROFILL)
	      && sectype != usectype)
	    as_bad (_("cannot overide zerofill section type for `%s,%s'"),
		    segname, sectname);
	  else
	    sectype = usectype;
	}
      secattr = xlat->macho_secattr;
      secalign = xlat->sectalign;
      flags = xlat->bfd_flags;
    }
  else
    {
      /* There is no normal BFD section name for this section.  Create one.
         The name created doesn't really matter as it will never be written
         on disk.  */
      size_t seglen = strlen (segname);
      size_t sectlen = strlen (sectname);
      char *n;

      n = xmalloc (seglen + 1 + sectlen + 1);
      memcpy (n, segname, seglen);
      n[seglen] = '.';
      memcpy (n + seglen + 1, sectname, sectlen);
      n[seglen + 1 + sectlen] = 0;
      name = n;
      if (specified_mask & SECT_TYPE_SPECIFIED)
	sectype = usectype;
      else
	sectype = BFD_MACH_O_S_REGULAR;
      secattr = BFD_MACH_O_S_ATTR_NONE;
      secalign = 0;
      flags = SEC_NO_FLAGS;
    }

  /* For now, just use what the user provided.  */

  if (specified_mask & SECT_ATTR_SPECIFIED)
    secattr = usecattr;

  if (specified_mask & SECT_ALGN_SPECIFIED)
    secalign = ualign;

  /* Sub-segments don't exists as is on Mach-O.  */
  sec = subseg_new (name, 0);

  oldflags = bfd_get_section_flags (stdoutput, sec);
  msect = bfd_mach_o_get_mach_o_section (sec);

  if (oldflags == SEC_NO_FLAGS)
    {
      /* New, so just use the defaults or what's specified.  */
      if (! bfd_set_section_flags (stdoutput, sec, flags))
	as_warn (_("failed to set flags for \"%s\": %s"),
		 bfd_section_name (stdoutput, sec),
		 bfd_errmsg (bfd_get_error ()));
 
      strncpy (msect->segname, segname, sizeof (msect->segname));
      strncpy (msect->sectname, sectname, sizeof (msect->sectname));

      msect->align = secalign;
      msect->flags = sectype | secattr;
      msect->reserved2 = stub_size;
      
      if (sectype == BFD_MACH_O_S_ZEROFILL
	  || sectype == BFD_MACH_O_S_GB_ZEROFILL)
        seg_info (sec)->bss = 1;
    }
  else if (flags != SEC_NO_FLAGS)
    {
      if (flags != oldflags
	  || msect->flags != (secattr | sectype))
	as_warn (_("Ignoring changed section attributes for %s"), name);
    }

  return sec;
}

/* .section

   The '.section' specification syntax looks like:
   .section <segment> , <section> [, type [, attribs [, size]]]

   White space is allowed everywhere between elements.

   <segment> and <section> may be from 0 to 16 chars in length - they may
   contain spaces but leading and trailing space will be trimmed.  It is 
   mandatory that they be present (or that zero-length names are indicated
   by ",,").

   There is only a single section type for any entry.

   There may be multiple attributes, they are delimited by `+'.

   Not all section types and attributes are accepted by the Darwin system
   assemblers as user-specifiable - although, at present, we do here.  */

static void
obj_mach_o_section (int ignore ATTRIBUTE_UNUSED)
{
  unsigned int sectype = BFD_MACH_O_S_REGULAR;
  unsigned int specified_mask = 0;
  unsigned int secattr = 0;
  offsetT sizeof_stub = 0;
  segT new_seg;
  char segname[17];
  char sectname[17];

#ifdef md_flush_pending_output
  md_flush_pending_output ();
#endif

  /* Get the User's segment annd section names.  */
  if (! obj_mach_o_get_section_names (segname, sectname, 17, 17))
    return;

  /* Parse section type, if present.  */
  if (*input_line_pointer == ',')
    {
      char *p;
      char c;
      char tmpc;
      int len;
      input_line_pointer++;
      SKIP_WHITESPACE ();
      p = input_line_pointer;
      while ((c = *input_line_pointer) != ','
	      && !is_end_of_line[(unsigned char) c])
	input_line_pointer++;

      len = input_line_pointer - p;
      /* strip trailing spaces.  */
      while (len > 0 && p[len-1] == ' ')
	len--;
      tmpc = p[len];

      /* Temporarily make a string from the token.  */
      p[len] = 0;
      sectype = bfd_mach_o_get_section_type_from_name (stdoutput, p);
      if (sectype > 255) /* Max Section ID == 255.  */
        {
          as_bad (_("unknown or invalid section type '%s'"), p);
	  p[len] = tmpc;
	  ignore_rest_of_line ();
	  return;
        }
      else
	specified_mask |= SECT_TYPE_SPECIFIED;
      /* Restore.  */
      p[len] = tmpc;

      /* Parse attributes.
	 TODO: check validity of attributes for section type.  */
      if ((specified_mask & SECT_TYPE_SPECIFIED)
	  && c == ',')
        {
          do
            {
              int attr;

	      /* Skip initial `,' and subsequent `+'.  */
              input_line_pointer++;
	      SKIP_WHITESPACE ();
	      p = input_line_pointer;
	      while ((c = *input_line_pointer) != '+'
		      && c != ','
		      && !is_end_of_line[(unsigned char) c])
		input_line_pointer++;

	      len = input_line_pointer - p;
	      /* strip trailing spaces.  */
	      while (len > 0 && p[len-1] == ' ')
		len--;
	      tmpc = p[len];

	      /* Temporarily make a string from the token.  */
	      p[len] ='\0';
              attr = bfd_mach_o_get_section_attribute_from_name (p);
	      if (attr == -1)
		{
                  as_bad (_("unknown or invalid section attribute '%s'"), p);
		  p[len] = tmpc;
		  ignore_rest_of_line ();
		  return;
                }
              else
		{
		  specified_mask |= SECT_ATTR_SPECIFIED;
                  secattr |= attr;
		}
	      /* Restore.  */
	      p[len] = tmpc;
            }
          while (*input_line_pointer == '+');

          /* Parse sizeof_stub.  */
          if ((specified_mask & SECT_ATTR_SPECIFIED) 
	      && *input_line_pointer == ',')
            {
              if (sectype != BFD_MACH_O_S_SYMBOL_STUBS)
                {
		  as_bad (_("unexpected section size information"));
		  ignore_rest_of_line ();
		  return;
		}

	      input_line_pointer++;
              sizeof_stub = get_absolute_expression ();
            }
          else if ((specified_mask & SECT_ATTR_SPECIFIED) 
		   && sectype == BFD_MACH_O_S_SYMBOL_STUBS)
            {
              as_bad (_("missing sizeof_stub expression"));
	      ignore_rest_of_line ();
	      return;
            }
        }
    }

  new_seg = obj_mach_o_make_or_get_sect (segname, sectname, specified_mask, 
					 sectype, secattr, 0 /*align */,
					 sizeof_stub);
  if (new_seg != NULL)
    {
      subseg_set (new_seg, 0);
      demand_empty_rest_of_line ();
    }
}

/* .zerofill segname, sectname [, symbolname, size [, align]]

   Zerofill switches, temporarily, to a sect of type 'zerofill'.

   If a variable name is given, it defines that in the section.
   Otherwise it just creates the section if it doesn't exist.  */

static void
obj_mach_o_zerofill (int ignore ATTRIBUTE_UNUSED)
{
  char segname[17];
  char sectname[17];
  segT old_seg = now_seg;
  segT new_seg;
  symbolS *sym = NULL;
  unsigned int align = 0;
  unsigned int specified_mask = 0;
  offsetT size;

#ifdef md_flush_pending_output
  md_flush_pending_output ();
#endif

  /* Get the User's segment annd section names.  */
  if (! obj_mach_o_get_section_names (segname, sectname, 17, 17))
    return;

  /* Parse variable definition, if present.  */
  if (*input_line_pointer == ',')
    {
      /* Parse symbol, size [.align] 
         We follow the method of s_common_internal, with the difference
         that the symbol cannot be a duplicate-common.  */
      char *name;
      char c;
      char *p;
      expressionS exp;
  
      input_line_pointer++; /* Skip ',' */
      SKIP_WHITESPACE ();
      name = input_line_pointer;
      c = get_symbol_end ();
      /* Just after name is now '\0'.  */
      p = input_line_pointer;
      *p = c;

      if (name == p)
	{
	  as_bad (_("expected symbol name"));
	  ignore_rest_of_line ();
	  goto done;
	}

      SKIP_WHITESPACE ();  
      if (*input_line_pointer == ',')
	input_line_pointer++;

      expression_and_evaluate (&exp);
      if (exp.X_op != O_constant
	  && exp.X_op != O_absent)
	{
	    as_bad (_("bad or irreducible absolute expression"));
	  ignore_rest_of_line ();
	  goto done;
	}
      else if (exp.X_op == O_absent)
	{
	  as_bad (_("missing size expression"));
	  ignore_rest_of_line ();
	  goto done;
	}

      size = exp.X_add_number;
      size &= ((offsetT) 2 << (stdoutput->arch_info->bits_per_address - 1)) - 1;
      if (exp.X_add_number != size || !exp.X_unsigned)
	{
	  as_warn (_("size (%ld) out of range, ignored"),
		   (long) exp.X_add_number);
	  ignore_rest_of_line ();
	  goto done;
	}

     *p = 0; /* Make the name into a c string for err messages.  */
     sym = symbol_find_or_make (name);
     if (S_IS_DEFINED (sym) || symbol_equated_p (sym))
	{
	  as_bad (_("symbol `%s' is already defined"), name);
	  *p = c;
	  ignore_rest_of_line ();
	   goto done;
	}

      size = S_GET_VALUE (sym);
      if (size == 0)
	size = exp.X_add_number;
      else if (size != exp.X_add_number)
	as_warn (_("size of \"%s\" is already %ld; not changing to %ld"),
		   name, (long) size, (long) exp.X_add_number);

      *p = c;  /* Restore the termination char.  */
      
      SKIP_WHITESPACE ();  
      if (*input_line_pointer == ',')
	{
	  align = (unsigned int) parse_align (0);
	  if (align == (unsigned int) -1)
	    {
	      as_warn (_("align value not recognized, using size"));
	      align = size;
	    }
	  if (align > 15)
	    {
	      as_warn (_("Alignment (%lu) too large: 15 assumed."),
			(unsigned long)align);
	      align = 15;
	    }
	  specified_mask |= SECT_ALGN_SPECIFIED;
	}
    }
 /* else just a section definition.  */

  specified_mask |= SECT_TYPE_SPECIFIED;
  new_seg = obj_mach_o_make_or_get_sect (segname, sectname, specified_mask, 
					 BFD_MACH_O_S_ZEROFILL,
					 BFD_MACH_O_S_ATTR_NONE,
					 align, (offsetT) 0 /*stub size*/);
  if (new_seg == NULL)
    return;

  /* In case the user specifies the bss section by mach-o name.
     Create it on demand */
  if (strcmp (new_seg->name, BSS_SECTION_NAME) == 0
      && bss_section == NULL)
    bss_section = new_seg;

  subseg_set (new_seg, 0);

  if (sym != NULL)
    {
      char *pfrag;

      if (align)
	{
	  record_alignment (new_seg, align);
	  frag_align (align, 0, 0);
	}

      /* Detach from old frag.  */
      if (S_GET_SEGMENT (sym) == new_seg)
	symbol_get_frag (sym)->fr_symbol = NULL;

      symbol_set_frag (sym, frag_now);
      pfrag = frag_var (rs_org, 1, 1, 0, sym, size, NULL);
      *pfrag = 0;

      S_SET_SEGMENT (sym, new_seg);
      if (new_seg == bss_section)
	S_CLEAR_EXTERNAL (sym);
    }

done:
  /* switch back to the section that was current before the .zerofill.  */
  subseg_set (old_seg, 0);
}

static segT 
obj_mach_o_segT_from_bfd_name (const char *nam, int must_succeed)
{
  const mach_o_section_name_xlat *xlat;
  const char *segn;
  segT sec;

  /* BFD has tables of flags and default attributes for all the sections that
     have a 'canonical' name.  */
  xlat = bfd_mach_o_section_data_for_bfd_name (stdoutput, nam, &segn);
  if (xlat == NULL)
    {
      if (must_succeed)
	as_fatal (_("BFD is out of sync with GAS, "
		     "unhandled well-known section type `%s'"), nam);
      return NULL;
    }

  sec = bfd_get_section_by_name (stdoutput, nam);
  if (sec == NULL)
    {
      bfd_mach_o_section *msect;

      sec = subseg_force_new (xlat->bfd_name, 0);

      /* Set default type, attributes and alignment.  */
      msect = bfd_mach_o_get_mach_o_section (sec);
      msect->flags = xlat->macho_sectype | xlat->macho_secattr;
      msect->align = xlat->sectalign;

      if ((msect->flags & BFD_MACH_O_SECTION_TYPE_MASK) 
	  == BFD_MACH_O_S_ZEROFILL)
	seg_info (sec)->bss = 1;
    }

  return sec;
}

static const char * const known_sections[] =
{
  /*  0 */ NULL,
  /* __TEXT */
  /*  1 */ ".const",
  /*  2 */ ".static_const",
  /*  3 */ ".cstring",
  /*  4 */ ".literal4",
  /*  5 */ ".literal8",
  /*  6 */ ".literal16",
  /*  7 */ ".constructor",
  /*  8 */ ".destructor",
  /*  9 */ ".eh_frame",
  /* __DATA */
  /* 10 */ ".const_data",
  /* 11 */ ".static_data",
  /* 12 */ ".mod_init_func",
  /* 13 */ ".mod_term_func",
  /* 14 */ ".dyld",
  /* 15 */ ".cfstring"
};

/* Interface for a known non-optional section directive.  */

static void
obj_mach_o_known_section (int sect_index)
{
  segT section;

#ifdef md_flush_pending_output
  md_flush_pending_output ();
#endif

  section = obj_mach_o_segT_from_bfd_name (known_sections[sect_index], 1);
  if (section != NULL)
    subseg_set (section, 0);

  /* else, we leave the section as it was; there was a fatal error anyway.  */
}

static const char * const objc_sections[] =
{
  /*  0 */ NULL,
  /*  1 */ ".objc_class",
  /*  2 */ ".objc_meta_class",
  /*  3 */ ".objc_cat_cls_meth",
  /*  4 */ ".objc_cat_inst_meth",
  /*  5 */ ".objc_protocol",
  /*  6 */ ".objc_string_object",
  /*  7 */ ".objc_cls_meth",
  /*  8 */ ".objc_inst_meth",
  /*  9 */ ".objc_cls_refs",
  /* 10 */ ".objc_message_refs",
  /* 11 */ ".objc_symbols",
  /* 12 */ ".objc_category",
  /* 13 */ ".objc_class_vars",
  /* 14 */ ".objc_instance_vars",
  /* 15 */ ".objc_module_info",
  /* 16 */ ".cstring", /* objc_class_names Alias for .cstring */
  /* 17 */ ".cstring", /* Alias objc_meth_var_types for .cstring */
  /* 18 */ ".cstring", /* objc_meth_var_names Alias for .cstring */
  /* 19 */ ".objc_selector_strs",
  /* 20 */ ".objc_image_info", /* extension.  */
  /* 21 */ ".objc_selector_fixup", /* extension.  */
  /* 22 */ ".objc1_class_ext", /* ObjC-1 extension.  */
  /* 23 */ ".objc1_property_list", /* ObjC-1 extension.  */
  /* 24 */ ".objc1_protocol_ext" /* ObjC-1 extension.  */
};

/* This currently does the same as known_sections, but kept separate for
   ease of maintenance.  */

static void
obj_mach_o_objc_section (int sect_index)
{
  segT section;
  
#ifdef md_flush_pending_output
  md_flush_pending_output ();
#endif

  section = obj_mach_o_segT_from_bfd_name (objc_sections[sect_index], 1);
  if (section != NULL)
    {
      obj_mach_o_seen_objc_section = 1; /* We need to ensure that certain
					   sections are present and in the
					   right order.  */
      subseg_set (section, 0);
    }

  /* else, we leave the section as it was; there was a fatal error anyway.  */
}

/* Debug section directives.  */

static const char * const debug_sections[] =
{
  /*  0 */ NULL,
  /* __DWARF */
  /*  1 */ ".debug_frame",
  /*  2 */ ".debug_info",
  /*  3 */ ".debug_abbrev",
  /*  4 */ ".debug_aranges",
  /*  5 */ ".debug_macinfo",
  /*  6 */ ".debug_line",
  /*  7 */ ".debug_loc",
  /*  8 */ ".debug_pubnames",
  /*  9 */ ".debug_pubtypes",
  /* 10 */ ".debug_str",
  /* 11 */ ".debug_ranges",
  /* 12 */ ".debug_macro"
};

/* ??? Maybe these should be conditional on gdwarf-*.
   It`s also likely that we will need to be able to set them from the cfi
   code.  */

static void
obj_mach_o_debug_section (int sect_index)
{
  segT section;

#ifdef md_flush_pending_output
  md_flush_pending_output ();
#endif

  section = obj_mach_o_segT_from_bfd_name (debug_sections[sect_index], 1);
  if (section != NULL)
    subseg_set (section, 0);

  /* else, we leave the section as it was; there was a fatal error anyway.  */
}

/* This could be moved to the tc-xx files, but there is so little dependency
   there, that the code might as well be shared.  */

struct opt_tgt_sect 
{
 const char *name;
 unsigned x86_val;
 unsigned ppc_val;
};

/* The extensions here are for specific sections that are generated by GCC
   and Darwin system tools, but don't have directives in the `system as'.  */

static const struct opt_tgt_sect tgt_sections[] =
{
  /*  0 */ { NULL, 0, 0},
  /*  1 */ { ".lazy_symbol_pointer", 0, 0},
  /*  2 */ { ".lazy_symbol_pointer2", 0, 0}, /* X86 - extension */
  /*  3 */ { ".lazy_symbol_pointer3", 0, 0}, /* X86 - extension */
  /*  4 */ { ".non_lazy_symbol_pointer", 0, 0},
  /*  5 */ { ".non_lazy_symbol_pointer_x86", 0, 0}, /* X86 - extension */
  /*  6 */ { ".symbol_stub", 16, 20},
  /*  7 */ { ".symbol_stub1", 0, 16}, /* PPC - extension */
  /*  8 */ { ".picsymbol_stub", 26, 36},
  /*  9 */ { ".picsymbol_stub1", 0, 32}, /* PPC - extension */
  /* 10 */ { ".picsymbol_stub2", 25, 0}, /* X86 - extension */
  /* 11 */ { ".picsymbol_stub3", 5, 0}, /* X86 - extension  */
};

/* Interface for an optional section directive.  */

static void
obj_mach_o_opt_tgt_section (int sect_index)
{
  const struct opt_tgt_sect *tgtsct = &tgt_sections[sect_index];
  segT section;

#ifdef md_flush_pending_output
  md_flush_pending_output ();
#endif

  section = obj_mach_o_segT_from_bfd_name (tgtsct->name, 0);
  if (section == NULL)
    {
      as_bad (_("%s is not used for the selected target"), tgtsct->name);
      /* Leave the section as it is.  */
    }
  else
    {
      bfd_mach_o_section *mo_sec = bfd_mach_o_get_mach_o_section (section);
      subseg_set (section, 0);
#if defined (TC_I386)
      mo_sec->reserved2 = tgtsct->x86_val;
#elif defined (TC_PPC)
      mo_sec->reserved2 = tgtsct->ppc_val;
#else
      mo_sec->reserved2 = 0;
#endif
    }
}

/* We don't necessarily have the three 'base' sections on mach-o.
   Normally, we would start up with only the 'text' section defined.
   However, even that can be suppressed with (TODO) c/l option "-n".
   Thus, we have to be able to create all three sections on-demand.  */

static void
obj_mach_o_base_section (int sect_index)
{
  segT section;

#ifdef md_flush_pending_output
  md_flush_pending_output ();
#endif

  /* We don't support numeric (or any other) qualifications on the
     well-known section shorthands.  */
  demand_empty_rest_of_line ();

  switch (sect_index)
    {
      /* Handle the three sections that are globally known within GAS.
	 For Mach-O, these are created on demand rather than at startup.  */
      case 1:
	if (text_section == NULL)
	  text_section = obj_mach_o_segT_from_bfd_name (TEXT_SECTION_NAME, 1);
	if (obj_mach_o_is_static)
	  {
	    bfd_mach_o_section *mo_sec
		= bfd_mach_o_get_mach_o_section (text_section);
	    mo_sec->flags &= ~BFD_MACH_O_S_ATTR_PURE_INSTRUCTIONS;
	  }
	section = text_section;
	break;
      case 2:
	if (data_section == NULL)
	  data_section = obj_mach_o_segT_from_bfd_name (DATA_SECTION_NAME, 1);
	section = data_section;
	break;
      case 3:
        /* ??? maybe this achieves very little, as an addition.  */
	if (bss_section == NULL)
	  {
	    bss_section = obj_mach_o_segT_from_bfd_name (BSS_SECTION_NAME, 1);
	    seg_info (bss_section)->bss = 1;
	  }
	section = bss_section;
	break;
      default:
        as_fatal (_("internal error: base section index out of range"));
        return;
	break;
    }
  subseg_set (section, 0);
}

/* This finishes off parsing a .comm or .lcomm statement, which both can have
   an (optional) alignment field.  It also allows us to create the bss section
   on demand.  */

static symbolS *
obj_mach_o_common_parse (int is_local, symbolS *symbolP,
			 addressT size)
{
  addressT align = 0;

  SKIP_WHITESPACE ();  

  /* Both comm and lcomm take an optional alignment, as a power
     of two between 1 and 15.  */
  if (*input_line_pointer == ',')
    {
      /* We expect a power of 2.  */
      align = parse_align (0);
      if (align == (addressT) -1)
	return NULL;
      if (align > 15)
	{
	  as_warn (_("Alignment (%lu) too large: 15 assumed."),
		  (unsigned long)align);
	  align = 15;
	}
    }

  if (is_local)
    {
      /* Create the BSS section on demand.  */
      if (bss_section == NULL)
	{
	  bss_section = obj_mach_o_segT_from_bfd_name (BSS_SECTION_NAME, 1);
	  seg_info (bss_section)->bss = 1;	  
	}
      bss_alloc (symbolP, size, align);
      S_CLEAR_EXTERNAL (symbolP);
    }
  else
    {
      S_SET_VALUE (symbolP, size);
      S_SET_ALIGN (symbolP, align);
      S_SET_EXTERNAL (symbolP);
      S_SET_SEGMENT (symbolP, bfd_com_section_ptr);
    }

  symbol_get_bfdsym (symbolP)->flags |= BSF_OBJECT;

  return symbolP;
}

static void
obj_mach_o_comm (int is_local)
{
  s_comm_internal (is_local, obj_mach_o_common_parse);
}

/* Set properties that apply to the whole file.  At present, the only
   one defined, is subsections_via_symbols.  */

typedef enum obj_mach_o_file_properties {
  OBJ_MACH_O_FILE_PROP_NONE = 0,
  OBJ_MACH_O_FILE_PROP_SUBSECTS_VIA_SYMS,
  OBJ_MACH_O_FILE_PROP_MAX
} obj_mach_o_file_properties;

static void 
obj_mach_o_fileprop (int prop)
{
  if (prop < 0 || prop >= OBJ_MACH_O_FILE_PROP_MAX)
    as_fatal (_("internal error: bad file property ID %d"), prop);
    
  switch ((obj_mach_o_file_properties) prop)
    {
      case OBJ_MACH_O_FILE_PROP_SUBSECTS_VIA_SYMS:
        obj_mach_o_subsections_by_symbols = 1;
	if (!bfd_set_private_flags (stdoutput, 
				    BFD_MACH_O_MH_SUBSECTIONS_VIA_SYMBOLS))
	  as_bad (_("failed to set subsections by symbols"));
	demand_empty_rest_of_line ();
	break;
      default:
	break;
    }
}

/* Dummy function to allow test-code to work while we are working
   on things.  */

static void
obj_mach_o_placeholder (int arg ATTRIBUTE_UNUSED)
{
  ignore_rest_of_line ();
}

const pseudo_typeS mach_o_pseudo_table[] =
{
  /* Section directives.  */
  { "comm", obj_mach_o_comm, 0 },
  { "lcomm", obj_mach_o_comm, 1 },

  { "text", obj_mach_o_base_section, 1},
  { "data", obj_mach_o_base_section, 2},
  { "bss", obj_mach_o_base_section, 3},   /* extension */

  { "const", obj_mach_o_known_section, 1},
  { "static_const", obj_mach_o_known_section, 2},
  { "cstring", obj_mach_o_known_section, 3},
  { "literal4", obj_mach_o_known_section, 4},
  { "literal8", obj_mach_o_known_section, 5},
  { "literal16", obj_mach_o_known_section, 6},
  { "constructor", obj_mach_o_known_section, 7},
  { "destructor", obj_mach_o_known_section, 8},
  { "eh_frame", obj_mach_o_known_section, 9},

  { "const_data", obj_mach_o_known_section, 10},
  { "static_data", obj_mach_o_known_section, 11},
  { "mod_init_func", obj_mach_o_known_section, 12},
  { "mod_term_func", obj_mach_o_known_section, 13},
  { "dyld", obj_mach_o_known_section, 14},
  { "cfstring", obj_mach_o_known_section, 15},

  { "objc_class", obj_mach_o_objc_section, 1},
  { "objc_meta_class", obj_mach_o_objc_section, 2},
  { "objc_cat_cls_meth", obj_mach_o_objc_section, 3},
  { "objc_cat_inst_meth", obj_mach_o_objc_section, 4},
  { "objc_protocol", obj_mach_o_objc_section, 5},
  { "objc_string_object", obj_mach_o_objc_section, 6},
  { "objc_cls_meth", obj_mach_o_objc_section, 7},
  { "objc_inst_meth", obj_mach_o_objc_section, 8},
  { "objc_cls_refs", obj_mach_o_objc_section, 9},
  { "objc_message_refs", obj_mach_o_objc_section, 10},
  { "objc_symbols", obj_mach_o_objc_section, 11},
  { "objc_category", obj_mach_o_objc_section, 12},
  { "objc_class_vars", obj_mach_o_objc_section, 13},
  { "objc_instance_vars", obj_mach_o_objc_section, 14},
  { "objc_module_info", obj_mach_o_objc_section, 15},
  { "objc_class_names", obj_mach_o_objc_section, 16}, /* Alias for .cstring */
  { "objc_meth_var_types", obj_mach_o_objc_section, 17}, /* Alias for .cstring */
  { "objc_meth_var_names", obj_mach_o_objc_section, 18}, /* Alias for .cstring */
  { "objc_selector_strs", obj_mach_o_objc_section, 19},
  { "objc_image_info", obj_mach_o_objc_section, 20}, /* extension.  */
  { "objc_selector_fixup", obj_mach_o_objc_section, 21}, /* extension.  */
  { "objc1_class_ext", obj_mach_o_objc_section, 22}, /* ObjC-1 extension.  */
  { "objc1_property_list", obj_mach_o_objc_section, 23}, /* ObjC-1 extension.  */
  { "objc1_protocol_ext", obj_mach_o_objc_section, 24}, /* ObjC-1 extension.  */

  { "debug_frame", obj_mach_o_debug_section, 1}, /* extension.  */
  { "debug_info", obj_mach_o_debug_section, 2}, /* extension.  */
  { "debug_abbrev", obj_mach_o_debug_section, 3}, /* extension.  */
  { "debug_aranges", obj_mach_o_debug_section, 4}, /* extension.  */
  { "debug_macinfo", obj_mach_o_debug_section, 5}, /* extension.  */
  { "debug_line", obj_mach_o_debug_section, 6}, /* extension.  */
  { "debug_loc", obj_mach_o_debug_section, 7}, /* extension.  */
  { "debug_pubnames", obj_mach_o_debug_section, 8}, /* extension.  */
  { "debug_pubtypes", obj_mach_o_debug_section, 9}, /* extension.  */
  { "debug_str", obj_mach_o_debug_section, 10}, /* extension.  */
  { "debug_ranges", obj_mach_o_debug_section, 11}, /* extension.  */
  { "debug_macro", obj_mach_o_debug_section, 12}, /* extension.  */
  
  { "lazy_symbol_pointer", obj_mach_o_opt_tgt_section, 1},
  { "lazy_symbol_pointer2", obj_mach_o_opt_tgt_section, 2}, /* extension.  */
  { "lazy_symbol_pointer3", obj_mach_o_opt_tgt_section, 3}, /* extension.  */
  { "non_lazy_symbol_pointer", obj_mach_o_opt_tgt_section, 4},
  { "non_lazy_symbol_pointer_x86", obj_mach_o_opt_tgt_section, 5}, /* extension.  */
  { "symbol_stub", obj_mach_o_opt_tgt_section, 6},
  { "symbol_stub1", obj_mach_o_opt_tgt_section, 7}, /* extension.  */
  { "picsymbol_stub", obj_mach_o_opt_tgt_section, 8}, /* extension.  */
  { "picsymbol_stub1", obj_mach_o_opt_tgt_section, 9}, /* extension.  */
  { "picsymbol_stub2", obj_mach_o_opt_tgt_section, 4}, /* extension.  */
  { "picsymbol_stub3", obj_mach_o_opt_tgt_section, 4}, /* extension.  */

  { "section", obj_mach_o_section, 0},
  { "zerofill", obj_mach_o_zerofill, 0},

  /* Symbol-related.  */
  { "indirect_symbol", obj_mach_o_placeholder, 0},
  { "weak_definition", obj_mach_o_placeholder, 0},
  { "private_extern", obj_mach_o_placeholder, 0},
  { "weak", obj_mach_o_weak, 0},   /* extension */

  /* File flags.  */
  { "subsections_via_symbols", obj_mach_o_fileprop, 
			       OBJ_MACH_O_FILE_PROP_SUBSECTS_VIA_SYMS},

  {NULL, NULL, 0}
};

/* Support stabs for mach-o.  */

void
obj_mach_o_process_stab (int what, const char *string,
			 int type, int other, int desc)
{
  symbolS *symbolP;
  bfd_mach_o_asymbol *s;

  switch (what)
    {
      case 'd':
	symbolP = symbol_new ("", now_seg, frag_now_fix (), frag_now);
	/* Special stabd NULL name indicator.  */
	S_SET_NAME (symbolP, NULL);
	break;

      case 'n':
      case 's':
	symbolP = symbol_new (string, undefined_section, (valueT) 0,
			      &zero_address_frag);
	pseudo_set (symbolP);
	break;

      default:
	as_bad(_("unrecognized stab type '%c'"), (char)what);
	abort ();
	break;
    }

  s = (bfd_mach_o_asymbol *) symbol_get_bfdsym (symbolP);
  s->n_type = type;
  s->n_desc = desc;
  /* For stabd, this will eventually get overwritten by the section number.  */
  s->n_sect = other;

  /* It's a debug symbol.  */
  s->symbol.flags |= BSF_DEBUGGING;
}