/*
** Copyright (c) 2020 D. Richard Hipp
**
** This program is free software; you can redistribute it and/or
** modify it under the terms of the Simplified BSD License (also
** known as the "2-Clause License" or "FreeBSD License".)
**
** This program is distributed in the hope that it will be useful,
** but without any warranty; without even the implied warranty of
** merchantability or fitness for a particular purpose.
**
** Author contact information:
**   drh@hwaci.com
**   http://www.hwaci.com/drh/
**
*******************************************************************************
**
** This file contains code for the /fileedit page and related bits.
*/
#include "config.h"
#include "fileedit.h"
#include <assert.h>
#include <stdarg.h>

/*
** State for the "mini-checkin" infrastructure, which enables the
** ability to commit changes to a single file without a checkout
** db, e.g. for use via an HTTP request.
**
** Use CheckinMiniInfo_init() to cleanly initialize one to a known
** valid/empty default state.
**
** Memory for all non-const pointer members is owned by the
** CheckinMiniInfo instance, unless explicitly noted otherwise, and is
** freed by CheckinMiniInfo_cleanup(). Similarly, each instance owns
** any memory for its own Blob members, but NOT for its pointers to
** blobs.
*/
struct CheckinMiniInfo {
  Manifest * pParent;  /* parent checkin. Memory is owned by this
                          object. */
  char *zParentUuid;   /* Full UUID of pParent */
  char *zFilename;     /* Name of single file to commit. Must be
                          relative to the top of the repo. */
  Blob fileContent;    /* Content of file referred to by zFilename. */
  Blob fileHash;       /* Hash of this->fileContent, using the repo's
                          preferred hash method. */
  Blob comment;        /* Check-in comment text */
  char *zCommentMimetype;  /* Mimetype of comment. May be NULL */
  char *zUser;         /* User name */
  char *zDate;         /* Optionally force this date string (anything
                          supported by date_in_standard_format()).
                          Maybe be NULL. */
  Blob *pMfOut;        /* If not NULL, checkin_mini() will write a
                          copy of the generated manifest here. This
                          memory is NOT owned by CheckinMiniInfo. */
  int filePerm;        /* Permissions (via file_perm()) of the input
                          file. We need to store this before calling
                          checkin_mini() because the real input file
                          name may differ from the repo-centric
                          this->zFilename, and checkin_mini() requires
                          the permissions of the original file. For
                          web commits, set this to PERM_REG or (when
                          editing executable scripts) PERM_EXE before
                          calling checkin_mini(). */
  int flags;           /* Bitmask of fossil_cimini_flags. */
};
typedef struct CheckinMiniInfo CheckinMiniInfo;

/*
** CheckinMiniInfo::flags values.
*/
enum fossil_cimini_flags {
/*
** Must have a value of 0. All other flags have unspecified values.
*/
CIMINI_NONE = 0,
/*
** Tells checkin_mini() to use dry-run mode.
*/
CIMINI_DRY_RUN = 1,
/*
** Tells checkin_mini() to allow forking from a non-leaf commit.
*/
CIMINI_ALLOW_FORK = 1<<1,
/*
** Tells checkin_mini() to dump its generated manifest to stdout.
*/
CIMINI_DUMP_MANIFEST = 1<<2,

/*
** By default, content containing what appears to be a merge conflict
** marker is not permitted. This flag relaxes that requirement.
*/
CIMINI_ALLOW_MERGE_MARKER = 1<<3,

/*
** By default mini-checkins are not allowed to be "older"
** than their parent. i.e. they may not have a timestamp
** which predates their parent. This flag bypasses that
** check.
*/
CIMINI_ALLOW_OLDER = 1<<4,

/*
** Indicates that the content of the newly-checked-in file is
** converted, if needed, to use the same EOL style as the previous
** version of that file. Only the in-memory/in-repo copies are
** affected, not the original file (if any).
*/
CIMINI_CONVERT_EOL_INHERIT = 1<<5,
/*
** Indicates that the input's EOLs should be converted to Unix-style.
*/
CIMINI_CONVERT_EOL_UNIX = 1<<6,
/*
** Indicates that the input's EOLs should be converted to Windows-style.
*/
CIMINI_CONVERT_EOL_WINDOWS = 1<<7,
/*
** A hint to checkin_mini() to "prefer" creation of a delta manifest.
** It may decide not to for various reasons.
*/
CIMINI_PREFER_DELTA = 1<<8,
/*
** A "stronger hint" to checkin_mini() to prefer creation of a delta
** manifest if it at all can. It will decide not to only if creation
** of a delta is not a realistic option or if it's forbitted by the
** forbid-delta-manifests repo config option. For this to work, it
** must be set together with the CIMINI_PREFER_DELTA flag, but the two
** cannot be combined in this enum.
**
** This option is ONLY INTENDED FOR TESTING, used in bypassing
** heuristics which may otherwise disable generation of a delta on the
** grounds of efficiency (e.g. not generating a delta if the parent
** non-delta only has a few F-cards).
*/
CIMINI_STRONGLY_PREFER_DELTA = 1<<9,
/*
** Tells checkin_mini() to permit the addition of a new file. Normally
** this is disabled because there are hypothetically many cases where
** it could cause the inadvertent addition of a new file when an
** update to an existing was intended, as a side-effect of name-case
** differences.
*/
CIMINI_ALLOW_NEW_FILE = 1<<10
};

/*
** Initializes p to a known-valid default state.
*/
static void CheckinMiniInfo_init( CheckinMiniInfo * p ){
  memset(p, 0, sizeof(CheckinMiniInfo));
  p->flags = CIMINI_NONE;
  p->filePerm = -1;
  p->comment = p->fileContent = p->fileHash = empty_blob;
}

/*
** Frees all memory owned by p, but does not free p.
 */
static void CheckinMiniInfo_cleanup( CheckinMiniInfo * p ){
  blob_reset(&p->comment);
  blob_reset(&p->fileContent);
  blob_reset(&p->fileHash);
  if(p->pParent){
    manifest_destroy(p->pParent);
  }
  fossil_free(p->zFilename);
  fossil_free(p->zDate);
  fossil_free(p->zParentUuid);
  fossil_free(p->zCommentMimetype);
  fossil_free(p->zUser);
  CheckinMiniInfo_init(p);
}

/*
** Internal helper which returns an F-card perms string suitable for
** writing as-is into a manifest. If it's not empty, it includes a
** leading space to separate it from the F-card's hash field.
*/
static const char * mfile_permint_mstring(int perm){
  switch(perm){
    case PERM_EXE: return " x";
    case PERM_LNK: return " l";
    default: return "";
  }
}

/*
** Given a ManifestFile permission string (or NULL), it returns one of
** PERM_REG, PERM_EXE, or PERM_LNK.
*/
static int mfile_permstr_int(const char *zPerm){
  if(!zPerm || !*zPerm) return PERM_REG;
  else if(strstr(zPerm,"x")) return PERM_EXE;
  else if(strstr(zPerm,"l")) return PERM_LNK;
  else return PERM_REG/*???*/;
}

/*
** Internal helper for checkin_mini() and friends. Appends an F-card
** for p to pOut.
*/
static void checkin_mini_append_fcard(Blob *pOut,
                                      const ManifestFile *p){
  if(p->zUuid){
    assert(*p->zUuid);
    blob_appendf(pOut, "F %F %s%s", p->zName,
                 p->zUuid,
                 mfile_permint_mstring(manifest_file_mperm(p)));
    if(p->zPrior){
      assert(*p->zPrior);
      blob_appendf(pOut, " %F\n", p->zPrior);
    }else{
      blob_append(pOut, "\n", 1);
    }
  }else{
    /* File was removed from parent delta. */
    blob_appendf(pOut, "F %F\n", p->zName);
  }
}

/*
** Handles the F-card parts for create_manifest_mini().
**
** If asDelta is true, F-cards will be handled as for a delta
** manifest, and the caller MUST have added a B-card to pOut before
** calling this.
**
** Returns 1 on success, 0 on error, and writes any error message to
** pErr (if it's not NULL). The only non-immediately-fatal/panic error
** is if pCI->filePerm is PERM_LNK or pCI would update a PERM_LNK
** in-repo file.
*/
static int create_manifest_mini_fcards( Blob * pOut,
                                        CheckinMiniInfo * pCI,
                                        int asDelta,
                                        Blob * pErr){
  int wroteThisCard = 0;
  const ManifestFile * pFile;
  int (*fncmp)(char const *, char const *) =  /* filename comparator */
    filenames_are_case_sensitive()
    ? fossil_strcmp
    : fossil_stricmp;
#define mf_err(EXPR) if(pErr) blob_appendf EXPR; return 0
#define write_this_card(NAME) \
  blob_appendf(pOut, "F %F %b%s\n", (NAME), &pCI->fileHash, \
               mfile_permint_mstring(pCI->filePerm)); \
  wroteThisCard = 1

  assert(pCI->filePerm!=PERM_LNK && "This should have been validated before.");
  assert(pCI->filePerm==PERM_REG || pCI->filePerm==PERM_EXE);
  if(PERM_LNK==pCI->filePerm){
    goto err_no_symlink;
  }
  manifest_file_rewind(pCI->pParent);
  if(asDelta!=0 && (pCI->pParent->zBaseline==0
                    || pCI->pParent->nFile==0)){
    /* Parent is a baseline or a delta with no F-cards, so this is
    ** the simplest case: create a delta with a single F-card.
    */
    pFile = manifest_file_find(pCI->pParent, pCI->zFilename);
    if(pFile!=0 && manifest_file_mperm(pFile)==PERM_LNK){
      goto err_no_symlink;
    }
    write_this_card(pFile ? pFile->zName : pCI->zFilename);
    return 1;
  }
  while(1){
    int cmp;
    if(asDelta==0){
      pFile = manifest_file_next(pCI->pParent, 0);
    }else{
      /* Parent is a delta manifest with F-cards. Traversal of delta
      ** manifest file entries is normally done via
      ** manifest_file_next(), which takes into account the
      ** differences between the delta and its parent and returns
      ** F-cards from both. Each successive delta from the same
      ** baseline includes all F-card changes from the previous
      ** deltas, so we instead clone the parent's F-cards except for
      ** the one (if any) which matches the new file.
      */
      pFile = pCI->pParent->iFile < pCI->pParent->nFile
        ? &pCI->pParent->aFile[pCI->pParent->iFile++]
        : 0;
    }
    if(0==pFile) break;
    cmp = fncmp(pFile->zName, pCI->zFilename);
    if(cmp<0){
      checkin_mini_append_fcard(pOut,pFile);
    }else{
      if(cmp==0 || 0==wroteThisCard){
        assert(0==wroteThisCard);
        if(PERM_LNK==manifest_file_mperm(pFile)){
          goto err_no_symlink;
        }
        write_this_card(cmp==0 ? pFile->zName : pCI->zFilename);
      }
      if(cmp>0){
        assert(wroteThisCard!=0);
        checkin_mini_append_fcard(pOut,pFile);
      }
    }
  }
  if(wroteThisCard==0){
    write_this_card(pCI->zFilename);
  }
  return 1;
err_no_symlink:
  mf_err((pErr,"Cannot commit or overwrite symlinks "
          "via mini-checkin."));
  return 0;
#undef write_this_card
#undef mf_err
}

/*
** Creates a manifest file, written to pOut, from the state in the
** fully-populated and semantically valid pCI argument. pCI is not
** *semantically* modified by this routine but cannot be const because
** blob_str() may need to NUL-terminate any given blob.
**
** Returns true on success. On error, returns 0 and, if pErr is not
** NULL, writes an error message there.
**
** Intended only to be called via checkin_mini() or routines which
** have already completely vetted pCI for semantic validity.
*/
static int create_manifest_mini( Blob * pOut, CheckinMiniInfo * pCI,
                                 Blob * pErr){
  Blob zCard = empty_blob;     /* Z-card checksum */
  int asDelta = 0;
#define mf_err(EXPR) if(pErr) blob_appendf EXPR; return 0

  assert(blob_str(&pCI->fileHash));
  assert(pCI->pParent);
  assert(pCI->zFilename);
  assert(pCI->zUser);
  assert(pCI->zDate);

  /* Potential TODOs include...
  **
  ** - Maybe add support for tags. Those can be edited via /info page,
  **   and feel like YAGNI/feature creep for this purpose.
  */
  blob_zero(pOut);
  manifest_file_rewind(pCI->pParent) /* force load of baseline */;
  /* Determine whether we want to create a delta manifest... */
  if((CIMINI_PREFER_DELTA & pCI->flags)
     && ((CIMINI_STRONGLY_PREFER_DELTA & pCI->flags)
         || (pCI->pParent->pBaseline
             ? pCI->pParent->pBaseline
             : pCI->pParent)->nFile > 15
         /* 15 is arbitrary: don't create a delta when there is only a
         ** tiny gain for doing so. That heuristic is not *quite*
         ** right, in that when we're deriving from another delta, we
         ** really should compare the F-card count between it and its
         ** baseline, and create a delta if the baseline has (say)
         ** twice or more as many F-cards as the previous delta. */)
     && !db_get_boolean("forbid-delta-manifests",0)
     ){
    asDelta = 1;
    blob_appendf(pOut, "B %s\n",
                 pCI->pParent->zBaseline
                 ? pCI->pParent->zBaseline
                 : pCI->zParentUuid);
  }
  blob_reserve(pOut, 1024 *
               (asDelta ? 2 : pCI->pParent->nFile/11+1
                /* In the fossil core repo, each 12-ish F-cards (on
                ** average) take up roughly 1kb */));
  if(blob_size(&pCI->comment)!=0){
    blob_appendf(pOut, "C %F\n", blob_str(&pCI->comment));
  }else{
    blob_append(pOut, "C (no\\scomment)\n", 16);
  }
  blob_appendf(pOut, "D %s\n", pCI->zDate);
  if(create_manifest_mini_fcards(pOut,pCI,asDelta,pErr)==0){
    return 0;
  }
  if(pCI->zCommentMimetype!=0 && pCI->zCommentMimetype[0]!=0){
    blob_appendf(pOut, "N %F\n", pCI->zCommentMimetype);
  }
  blob_appendf(pOut, "P %s\n", pCI->zParentUuid);
  blob_appendf(pOut, "U %F\n", pCI->zUser);
  md5sum_blob(pOut, &zCard);
  blob_appendf(pOut, "Z %b\n", &zCard);
  blob_reset(&zCard);
  return 1;
#undef mf_err
}

/*
** A so-called "single-file/mini/web checkin" is a slimmed-down form
** of the checkin command which accepts only a single file and is
** intended to accept edits to a file via the web interface or from
** the CLI from outside of a checkout.
**
** Being fully non-interactive is a requirement for this function,
** thus it cannot perform autosync or similar activities (which
** includes checking for repo locks).
**
** This routine uses the state from the given fully-populated pCI
** argument to add pCI->fileContent to the database, and create and
** save a manifest for that change. Ownership of pCI and its contents
** are unchanged.
**
** This function may may modify pCI as follows:
**
** - If one of Manifest pCI->pParent or pCI->zParentUuid are NULL,
**   then the other will be assigned based on its counterpart. Both
**   may not be NULL.
**
** - pCI->zDate is normalized to/replaced with a valid date/time
**   string. If its original value cannot be validated then
**   this function fails. If pCI->zDate is NULL, the current time
**   is used.
**
** - If the CIMINI_CONVERT_EOL_INHERIT flag is set,
**   pCI->fileContent appears to be plain text, and its line-ending
**   style differs from its previous version, it is converted to the
**   same EOL style as the previous version. If this is done, the
**   pCI->fileHash is re-computed. Note that only pCI->fileContent,
**   not the original file, is affected by the conversion.
**
** - Else if one of the CIMINI_CONVERT_EOL_WINDOWS or
**   CIMINI_CONVERT_EOL_UNIX flags are set, pCI->fileContent is
**   converted, if needed, to the corresponding EOL style.
**
** - If EOL conversion takes place, pCI->fileHash is re-calculated.
**
** - If pCI->fileHash is empty, this routine populates it with the
**   repository's preferred hash algorithm (after any EOL conversion).
**
** - pCI->comment may be converted to Unix-style newlines.
**
** pCI's ownership is not modified.
**
** This function validates pCI's state and fails if any validation
** fails.
**
** On error, returns false (0) and, if pErr is not NULL, writes a
** diagnostic message there.
** 
** Returns true on success. If pRid is not NULL, the RID of the
** resulting manifest is written to *pRid.
**
** The checkin process is largely influenced by pCI->flags, and that
** must be populated before calling this. See the fossil_cimini_flags
** enum for the docs for each flag.
*/
static int checkin_mini(CheckinMiniInfo * pCI, int *pRid, Blob * pErr){
  Blob mf = empty_blob;             /* output manifest */
  int rid = 0, frid = 0;            /* various RIDs */
  int isPrivate;                    /* whether this is private content
                                       or not */
  ManifestFile * zFilePrev;         /* file entry from pCI->pParent */
  int prevFRid = 0;                 /* RID of file's prev. version */
#define ci_err(EXPR) if(pErr!=0){blob_appendf EXPR;} goto ci_error

  db_begin_transaction();
  if(pCI->pParent==0 && pCI->zParentUuid==0){
    ci_err((pErr, "Cannot determine parent version."));
  }
  else if(pCI->pParent==0){
    pCI->pParent = manifest_get_by_name(pCI->zParentUuid, 0);
    if(pCI->pParent==0){
      ci_err((pErr,"Cannot load manifest for [%S].", pCI->zParentUuid));
    }
  }else if(pCI->zParentUuid==0){
    pCI->zParentUuid = rid_to_uuid(pCI->pParent->rid);
    assert(pCI->zParentUuid);
  }
  assert(pCI->pParent->rid>0);
  if(leaf_is_closed(pCI->pParent->rid)){
    ci_err((pErr,"Cannot commit to a closed leaf."));
    /* Remember that in order to override this we'd also need to
    ** cancel TAG_CLOSED on pCI->pParent. There would seem to be no
    ** reason we can't do that via the generated manifest, but the
    ** commit command does not offer that option, so mini-checkin
    ** probably shouldn't, either.
    */
  }
  if( !db_exists("SELECT 1 FROM user WHERE login=%Q", pCI->zUser) ){
    ci_err((pErr,"No such user: %s", pCI->zUser));
  }
  if(!(CIMINI_ALLOW_FORK & pCI->flags)
     && !is_a_leaf(pCI->pParent->rid)){
    ci_err((pErr,"Parent [%S] is not a leaf and forking is disabled.",
            pCI->zParentUuid));
  }
  if(!(CIMINI_ALLOW_MERGE_MARKER & pCI->flags)
     && contains_merge_marker(&pCI->fileContent)){
    ci_err((pErr,"Content appears to contain a merge conflict marker."));
  }
  if(!file_is_simple_pathname(pCI->zFilename, 1)){
    ci_err((pErr,"Invalid filename for use in a repository: %s",
            pCI->zFilename));
  }
  if(!(CIMINI_ALLOW_OLDER & pCI->flags)
     && !checkin_is_younger(pCI->pParent->rid, pCI->zDate)){
    ci_err((pErr,"Checkin time (%s) may not be older "
            "than its parent (%z).",
            pCI->zDate,
            db_text(0, "SELECT strftime('%%Y-%%m-%%dT%%H:%%M:%%f',%lf)",
                    pCI->pParent->rDate)
            ));
  }
  {
    /*
    ** Normalize the timestamp. We don't use date_in_standard_format()
    ** because that has side-effects we don't want to trigger here.
    */
    char * zDVal = db_text(
         0, "SELECT strftime('%%Y-%%m-%%dT%%H:%%M:%%f',%Q)",
         pCI->zDate ? pCI->zDate : "now");
    if(zDVal==0 || zDVal[0]==0){
      fossil_free(zDVal);
      ci_err((pErr,"Invalid timestamp string: %s", pCI->zDate));
    }
    fossil_free(pCI->zDate);
    pCI->zDate = zDVal;
  }
  { /* Confirm that only one EOL policy is in place. */
    int n = 0;
    if(CIMINI_CONVERT_EOL_INHERIT & pCI->flags) ++n;
    if(CIMINI_CONVERT_EOL_UNIX & pCI->flags) ++n;
    if(CIMINI_CONVERT_EOL_WINDOWS & pCI->flags) ++n;
    if(n>1){
      ci_err((pErr,"More than 1 EOL conversion policy was specified."));
    }
  }
  /* Potential TODOs include:
  **
  ** - Commit allows an empty checkin only with a flag, but we
  **   currently disallow an empty checkin entirely. Conform with
  **   commit?
  **
  ** Non-TODOs:
  **
  ** - Check for a commit lock would require auto-sync, which this
  **   code cannot do if it's going to be run via a web page.
  */

  /*
  ** Confirm that pCI->zFilename can be found in pCI->pParent.  If
  ** not, fail unless the CIMINI_ALLOW_NEW_FILE flag is set. This is
  ** admittedly an artificial limitation, not strictly necessary. We
  ** do it to hopefully reduce the chance of an "oops" where file
  ** X/Y/z gets committed as X/Y/Z or X/y/z due to a typo or
  ** case-sensitivity mismatch between the user/repo/filesystem, or
  ** some such.
  */
  manifest_file_rewind(pCI->pParent);
  zFilePrev = manifest_file_find(pCI->pParent, pCI->zFilename);
  if(!(CIMINI_ALLOW_NEW_FILE & pCI->flags)
     && (!zFilePrev
         || !zFilePrev->zUuid/*was removed from parent delta manifest*/)
     ){
    ci_err((pErr,"File [%s] not found in manifest [%S]. "
            "Adding new files is currently not permitted.",
            pCI->zFilename, pCI->zParentUuid));
  }else if(zFilePrev
           && manifest_file_mperm(zFilePrev)==PERM_LNK){
    ci_err((pErr,"Cannot save a symlink via a mini-checkin."));
  }
  if(zFilePrev){
    prevFRid = fast_uuid_to_rid(zFilePrev->zUuid);
  }

  if(((CIMINI_CONVERT_EOL_INHERIT & pCI->flags)
      || (CIMINI_CONVERT_EOL_UNIX & pCI->flags)
      || (CIMINI_CONVERT_EOL_WINDOWS & pCI->flags))
     && blob_size(&pCI->fileContent)>0
     ){
    /* Convert to the requested EOL style. Note that this inherently
    ** runs a risk of breaking content, e.g. string literals which
    ** contain embedded newlines. Note that HTML5 specifies that
    ** form-submitted TEXTAREA content gets normalized to CRLF-style:
    **
    ** https://html.spec.whatwg.org/multipage/form-elements.html#the-textarea-element
    */
    const int pseudoBinary = LOOK_LONG | LOOK_NUL;
    const int lookFlags = LOOK_CRLF | LOOK_LONE_LF | pseudoBinary;
    const int lookNew = looks_like_utf8( &pCI->fileContent, lookFlags );
    if(!(pseudoBinary & lookNew)){
      int rehash = 0;
      /*fossil_print("lookNew=%08x\n",lookNew);*/
      if(CIMINI_CONVERT_EOL_INHERIT & pCI->flags){
        Blob contentPrev = empty_blob;
        int lookOrig, nOrig;
        content_get(prevFRid, &contentPrev);
        lookOrig = looks_like_utf8(&contentPrev, lookFlags);
        nOrig = blob_size(&contentPrev);
        blob_reset(&contentPrev);
        /*fossil_print("lookOrig=%08x\n",lookOrig);*/
        if(nOrig>0 && lookOrig!=lookNew){
          /* If there is a newline-style mismatch, adjust the new
          ** content version to the previous style, then re-hash the
          ** content. Note that this means that what we insert is NOT
          ** what's in the filesystem.
          */
          if(!(lookOrig & LOOK_CRLF) && (lookNew & LOOK_CRLF)){
            /* Old has Unix-style, new has Windows-style. */
            blob_to_lf_only(&pCI->fileContent);
            rehash = 1;
          }else if((lookOrig & LOOK_CRLF) && !(lookNew & LOOK_CRLF)){
            /* Old has Windows-style, new has Unix-style. */
            blob_add_cr(&pCI->fileContent);
            rehash = 1;
          }
        }
      }else{
        const int oldSize = blob_size(&pCI->fileContent);
        if(CIMINI_CONVERT_EOL_UNIX & pCI->flags){
          if(LOOK_CRLF & lookNew){
            blob_to_lf_only(&pCI->fileContent);
          }
        }else{
          assert(CIMINI_CONVERT_EOL_WINDOWS & pCI->flags);
          if(!(LOOK_CRLF & lookNew)){
            blob_add_cr(&pCI->fileContent);
          }
        }
        if(blob_size(&pCI->fileContent)!=oldSize){
          rehash = 1;
        }
      }
      if(rehash!=0){
        hname_hash(&pCI->fileContent, 0, &pCI->fileHash);
      }
    }
  }/* end EOL conversion */

  if(blob_size(&pCI->fileHash)==0){
    /* Hash the content if it's not done already... */
    hname_hash(&pCI->fileContent, 0, &pCI->fileHash);
    assert(blob_size(&pCI->fileHash)>0);
  }
  if(zFilePrev){
    /* Has this file been changed since its previous commit?  Note
    ** that we have to delay this check until after the potentially
    ** expensive EOL conversion. */
    assert(blob_size(&pCI->fileHash));
    if(0==fossil_strcmp(zFilePrev->zUuid, blob_str(&pCI->fileHash))
       && manifest_file_mperm(zFilePrev)==pCI->filePerm){
      ci_err((pErr,"File is unchanged. Not committing."));
    }
  }
#if 1
  /* Do we really want to normalize comment EOLs? Web-posting will
  ** submit them in CRLF or LF format, depending on how exactly the
  ** content is submitted (FORM (CRLF) or textarea-to-POST (LF, at
  ** least in theory)). */
  blob_to_lf_only(&pCI->comment);
#endif
  /* Create, save, deltify, and crosslink the manifest... */
  if(create_manifest_mini(&mf, pCI, pErr)==0){
    return 0;
  }
  isPrivate = content_is_private(pCI->pParent->rid);
  rid = content_put_ex(&mf, 0, 0, 0, isPrivate);
  if(pCI->flags & CIMINI_DUMP_MANIFEST){
    fossil_print("%b", &mf);
  }
  if(pCI->pMfOut!=0){
    /* Cross-linking clears mf, so we have to copy it,
    ** instead of taking over its memory. */
    blob_reset(pCI->pMfOut);
    blob_append(pCI->pMfOut, blob_buffer(&mf), blob_size(&mf));
  }
  content_deltify(rid, &pCI->pParent->rid, 1, 0);
  manifest_crosslink(rid, &mf, 0);
  blob_reset(&mf);
  /* Save and deltify the file content... */
  frid = content_put_ex(&pCI->fileContent, blob_str(&pCI->fileHash),
                        0, 0, isPrivate);
  if(zFilePrev!=0){
    assert(prevFRid>0);
    content_deltify(frid, &prevFRid, 1, 0);
  }
  db_end_transaction((CIMINI_DRY_RUN & pCI->flags) ? 1 : 0);
  if(pRid!=0){
    *pRid = rid;
  }
  return 1;
ci_error:
  assert(db_transaction_nesting_depth()>0);
  db_end_transaction(1);
  return 0;
#undef ci_err
}

/*
** COMMAND: test-ci-mini
**
** This is an on-going experiment, subject to change or removal at
** any time.
**
** Usage: %fossil test-ci-mini ?OPTIONS? FILENAME
**
** where FILENAME is a repo-relative name as it would appear in the
** vfile table.
**
** Options:
**
**   --repository|-R REPO      The repository file to commit to.
**   --as FILENAME             The repository-side name of the input
**                             file, relative to the top of the
**                             repository. Default is the same as the
**                             input file name.
**   --comment|-m COMMENT      Required checkin comment.
**   --comment-file|-M FILE    Reads checkin comment from the given file.
**   --revision|-r VERSION     Commit from this version. Default is
**                             the checkout version (if available) or
**                             trunk (if used without a checkout).
**   --allow-fork              Allows the commit to be made against a
**                             non-leaf parent. Note that no autosync
**                             is performed beforehand.
**   --allow-merge-conflict    Allows checkin of a file even if it
**                             appears to contain a fossil merge conflict
**                             marker.
**   --user-override USER      USER to use instead of the current
**                             default.
**   --date-override DATETIME  DATE to use instead of 'now'.
**   --allow-older             Allow a commit to be older than its
**                             ancestor.
**   --convert-eol-inherit     Convert EOL style of the checkin to match
**                             the previous version's content.
**   --convert-eol-unix        Convert the EOL style to Unix.
**   --convert-eol-windows     Convert the EOL style to Windows.
**   (only one of the --convert-eol-X options may be used and they only
**    modified the saved blob, not the input file.)
**   --delta                   Prefer to generate a delta manifest, if
**                             able. The forbid-delta-manifests repo
**                             config option trumps this, as do certain
**                             heuristics.
**   --allow-new-file          Allow addition of a new file this way.
**                             Disabled by default to avoid that case-
**                             sensitivity errors inadvertently lead to
**                             adding a new file where an update is
**                             intended.
**   --dump-manifest|-d        Dumps the generated manifest to stdout
**                             immediately after it's generated.
**   --save-manifest FILE      Saves the generated manifest to a file
**                             after successfully processing it.
**   --wet-run                 Disables the default dry-run mode.
**
** Example:
**
** %fossil test-ci-mini -R REPO -m ... -r foo --as src/myfile.c myfile.c
**
*/
void test_ci_mini_cmd(void){
  CheckinMiniInfo cimi;       /* checkin state */
  int newRid = 0;                /* RID of new version */
  const char * zFilename;        /* argv[2] */
  const char * zComment;         /* -m comment */
  const char * zCommentFile;     /* -M FILE */
  const char * zAsFilename;      /* --as filename */
  const char * zRevision;        /* --revision|-r [=trunk|checkout] */
  const char * zUser;            /* --user-override */
  const char * zDate;            /* --date-override */
  char const * zManifestFile = 0;/* --save-manifest FILE */

  /* This function should perform only the minimal "business logic" it
  ** needs in order to fully/properly populate the CheckinMiniInfo and
  ** then pass it on to checkin_mini() to do most of the validation
  ** and work. The point of this is to avoid duplicate code when a web
  ** front-end is added for checkin_mini().
  */
  CheckinMiniInfo_init(&cimi);
  zComment = find_option("comment","m",1);
  zCommentFile = find_option("comment-file","M",1);
  zAsFilename = find_option("as",0,1);
  zRevision = find_option("revision","r",1);
  zUser = find_option("user-override",0,1);
  zDate = find_option("date-override",0,1);
  zManifestFile = find_option("save-manifest",0,1);
  if(find_option("wet-run",0,0)==0){
    cimi.flags |= CIMINI_DRY_RUN;
  }
  if(find_option("allow-fork",0,0)!=0){
    cimi.flags |= CIMINI_ALLOW_FORK;
  }
  if(find_option("dump-manifest","d",0)!=0){
    cimi.flags |= CIMINI_DUMP_MANIFEST;
  }
  if(find_option("allow-merge-conflict",0,0)!=0){
    cimi.flags |= CIMINI_ALLOW_MERGE_MARKER;
  }
  if(find_option("allow-older",0,0)!=0){
    cimi.flags |= CIMINI_ALLOW_OLDER;
  }
  if(find_option("convert-eol-inherit",0,0)!=0){
    cimi.flags |= CIMINI_CONVERT_EOL_INHERIT;
  }else if(find_option("convert-eol-unix",0,0)!=0){
    cimi.flags |= CIMINI_CONVERT_EOL_UNIX;
  }else if(find_option("convert-eol-windows",0,0)!=0){
    cimi.flags |= CIMINI_CONVERT_EOL_WINDOWS;
  }
  if(find_option("delta",0,0)!=0){
    cimi.flags |= CIMINI_PREFER_DELTA;
  }
  if(find_option("delta2",0,0)!=0){
    /* Undocumented. For testing only. */
    cimi.flags |= CIMINI_PREFER_DELTA | CIMINI_STRONGLY_PREFER_DELTA;
  }
  if(find_option("allow-new-file",0,0)!=0){
    cimi.flags |= CIMINI_ALLOW_NEW_FILE;
  }
  db_find_and_open_repository(0, 0);
  verify_all_options();
  user_select();
  if(g.argc!=3){
    usage("INFILE");
  }
  if(zComment && zCommentFile){
    fossil_fatal("Only one of -m or -M, not both, may be used.");
  }else{
    if(zCommentFile && *zCommentFile){
      blob_read_from_file(&cimi.comment, zCommentFile, ExtFILE);
    }else if(zComment && *zComment){
      blob_append(&cimi.comment, zComment, -1);
    }
    if(!blob_size(&cimi.comment)){
      fossil_fatal("Non-empty checkin comment is required.");
    }
  }
  db_begin_transaction();
  zFilename = g.argv[2];
  cimi.zFilename = mprintf("%/", zAsFilename ? zAsFilename : zFilename);
  cimi.filePerm = file_perm(zFilename, ExtFILE);
  cimi.zUser = mprintf("%s", zUser ? zUser : login_name());
  if(zDate){
    cimi.zDate = mprintf("%s", zDate);
  }
  if(zRevision==0 || zRevision[0]==0){
    if(g.localOpen/*checkout*/){
      zRevision = db_lget("checkout-hash", 0)/*leak*/;
    }else{
      zRevision = "trunk";
    }
  }
  name_to_uuid2(zRevision, "ci", &cimi.zParentUuid);
  if(cimi.zParentUuid==0){
    fossil_fatal("Cannot determine version to commit to.");
  }
  blob_read_from_file(&cimi.fileContent, zFilename, ExtFILE);
  {
    Blob theManifest = empty_blob; /* --save-manifest target */
    Blob errMsg = empty_blob;
    int rc;
    if(zManifestFile){
      cimi.pMfOut = &theManifest;
    }
    rc = checkin_mini(&cimi, &newRid, &errMsg);
    if(rc){
      assert(blob_size(&errMsg)==0);
    }else{
      assert(blob_size(&errMsg));
      fossil_fatal("%b", &errMsg);
    }
    if(zManifestFile){
      fossil_print("Writing manifest to: %s\n", zManifestFile);
      assert(blob_size(&theManifest)>0);
      blob_write_to_file(&theManifest, zManifestFile);
      blob_reset(&theManifest);
    }
  }
  if(newRid!=0){
    fossil_print("New version%s: %z\n",
                 (cimi.flags & CIMINI_DRY_RUN) ? " (dry run)" : "",
                 rid_to_uuid(newRid));
  }
  db_end_transaction(0/*checkin_mini() will have triggered it to roll
                      ** back in dry-run mode, but we need access to
                      ** the transaction-written db state in this
                      ** routine.*/);
  if(!(cimi.flags & CIMINI_DRY_RUN) && newRid!=0 && g.localOpen!=0){
    fossil_warning("The checkout state is now out of sync "
                   "with regards to this commit. It needs to be "
                   "'update'd or 'close'd and re-'open'ed.");
  }
  CheckinMiniInfo_cleanup(&cimi);
}

/*
** If the fileedit-glob setting has a value, this returns its Glob
** object (in memory owned by this function), else it returns NULL.
*/
Glob *fileedit_glob(void){
  static Glob * pGlobs = 0;
  static int once = 0;
  if(0==pGlobs && once==0){
    char * zGlobs = db_get("fileedit-glob",0);
    once = 1;
    if(0!=zGlobs && 0!=*zGlobs){
      pGlobs = glob_create(zGlobs);
    }
    fossil_free(zGlobs);
  }
  return pGlobs;
}

/*
** Returns true if the given filename qualifies for online editing by
** the current user, else returns false.
**
** Editing requires that the user have the Write permission and that
** the filename match the glob defined by the fileedit-glob setting.
** A missing or empty value for that glob disables all editing.
*/
int fileedit_is_editable(const char *zFilename){
  Glob * pGlobs = fileedit_glob();
  if(pGlobs!=0 && zFilename!=0 && *zFilename!=0 && 0!=g.perm.Write){
    return glob_match(pGlobs, zFilename);
  }else{
    return 0;
  }
}

enum fileedit_render_preview_flags {
FE_PREVIEW_LINE_NUMBERS = 1
};
enum fileedit_render_modes {
/* GUESS must be 0. All others have unspecified values. */
FE_RENDER_GUESS = 0,
FE_RENDER_PLAIN_TEXT,
FE_RENDER_HTML_IFRAME,
FE_RENDER_HTML_INLINE,
FE_RENDER_WIKI
};

static int fileedit_render_mode_for_mimetype(const char * zMimetype){
  int rc = FE_RENDER_PLAIN_TEXT;
  if( zMimetype ){
    if( fossil_strcmp(zMimetype, "text/html")==0 ){
      rc = FE_RENDER_HTML_IFRAME;
    }else if( fossil_strcmp(zMimetype, "text/x-fossil-wiki")==0
              || fossil_strcmp(zMimetype, "text/x-markdown")==0 ){
      rc = FE_RENDER_WIKI;
    }
  }
  return rc;
}

/*
** Performs the PREVIEW mode for /filepage.
**
** If *renderMode==FE_RENDER_GUESS then *renderMode gets set to the
** mode which is guessed at for the rendering.
*/
static void fileedit_render_preview(Blob * pContent,
                                    const char *zFilename,
                                    int flags, int * renderMode,
                                    int nIframeHeightEm){
  const char * zMime;
  zMime = mimetype_from_name(zFilename);
  if(FE_RENDER_GUESS==*renderMode){
    *renderMode = fileedit_render_mode_for_mimetype(zMime);
  }
  switch(*renderMode){
    case FE_RENDER_HTML_IFRAME:{
      char * z64 = encode64(blob_str(pContent), blob_size(pContent));
      CX("<iframe width='100%%' frameborder='0' "
         "marginwidth='0' style='height:%dem' "
         "marginheight='0' sandbox='allow-same-origin' "
         "id='ifm1' src='data:text/html;base64,%z'"
         "></iframe>",
         nIframeHeightEm ? nIframeHeightEm : 40,
         z64);
      break;
    }
    case FE_RENDER_HTML_INLINE:{
      CX("%b",pContent);
      break;
    }
    case FE_RENDER_WIKI:
      wiki_render_by_mimetype(pContent, zMime);
      break;
    default:{
      const char *zExt = strrchr(zFilename,'.');
      const char *zContent = blob_str(pContent);
      if(FE_PREVIEW_LINE_NUMBERS & flags){
        output_text_with_line_numbers(zContent, "on");
      }else if(zExt && zExt[1]){
        CX("<pre><code class='language-%s'>%h</code></pre>",
           zExt+1, zContent);
      }else{
        CX("<pre>%h</pre>", zContent);
      }
      break;
    }
  }
}

/*
** Renders diffs for the /fileedit page. pContent is the
** locally-edited content.  frid is the RID of the file's blob entry
** from which pContent is based. zManifestUuid is the checkin version
** to which RID belongs - it is purely informational, for labeling the
** diff view. isSbs is true for side-by-side diffs, false for unified.
*/
static void fileedit_render_diff(Blob * pContent, int frid,
                                 const char * zManifestUuid,
                                 u64 diffFlags){
  Blob orig = empty_blob;
  Blob out = empty_blob;

  content_get(frid, &orig);
  text_diff(&orig, pContent, &out, 0, diffFlags);
  if(blob_size(&out)==0){
    /* nothing to do */
  }else if(DIFF_SIDEBYSIDE & diffFlags){
    CX("%b",&out);
  }else{
    CX("<pre class='udiff'>%b</pre>",&out);
  }
  blob_reset(&orig);
  blob_reset(&out);
}

/*
** Given a repo-relative filename and a manifest RID, returns the UUID
** of the corresponding file entry.  Returns NULL if no match is
** found.  If pFilePerm is not NULL, the file's permission flag value
** is written to *pFilePerm.
*/
static char *fileedit_file_uuid(char const *zFilename,
                                int vid, int *pFilePerm){
  Stmt stmt = empty_Stmt;
  char * zFileUuid = 0;
  db_prepare(&stmt, "SELECT uuid, perm FROM files_of_checkin "
             "WHERE filename=%Q %s AND checkinID=%d",
             zFilename, filename_collation(), vid);
  if(SQLITE_ROW==db_step(&stmt)){
    zFileUuid = mprintf("%s",db_column_text(&stmt, 0));
    if(pFilePerm){
      *pFilePerm = mfile_permstr_int(db_column_text(&stmt, 1));
    }
  }
  db_finalize(&stmt);
  return zFileUuid;
}

/*
** Helper for /fileedit_xyz routes. Clears the CGI content buffer,
** sets an error status code, and queues up a JSON response in the
** form of an object:
**
** {error: formatted message}
**
** After calling this, the caller should immediately return.
 */
static void fileedit_ajax_error(int httpCode, const char * zFmt, ...){
  Blob msg = empty_blob;
  Blob content = empty_blob;
  va_list vargs;
  va_start(vargs,zFmt);
  blob_vappendf(&msg, zFmt, vargs);
  va_end(vargs);
  blob_appendf(&content,"{\"error\":%!j}", blob_str(&msg));
  blob_reset(&msg);
  cgi_set_content(&content);
  cgi_set_status(httpCode, "Error");
  cgi_set_content_type("application/json");
}

/*
** Performs bootstrapping common to the /fileedit_xyz AJAX routes.
** Returns 0 if bootstrapping fails (wrong permissions), in which
** case it has reported the error and the route should immediately
** return. Returns true on success.
**
** Must be passed true if the request being set up requires POST,
** else false.
*/
static int fileedit_ajax_boostrap(int requirePost){
  login_check_credentials();
  if( !g.perm.Write ){
    fileedit_ajax_error(403,"Write permissions required.");
    return 0;
  }else if(0==cgi_csrf_safe(requirePost)){
    fileedit_ajax_error(403,
                        "CSRF violation (make sure sending of HTTP "
                        "Referer headers is enabled for XHR "
                        "connections).");
    return 0;
  }
  return 1;
}
/*
** Returns true if the current user is allowed to edit the given
** filename, as determined by fileedit_is_editable(), else false,
** in which case it queues up an error response and the caller
** must return immediately.
*/
static int fileedit_ajax_check_filename(const char * zFilename){
  if(0==fileedit_is_editable(zFilename)){
    fileedit_ajax_error(403, "File is disallowed by the "
                        "fileedit-glob setting.");
    return 0;
  }
  return 1;
}


/*
** If zFn is not NULL, it is assigned the value of the first one of
** the "filename" or "fn" CGI parameters which is set.
**
** If zCi is not NULL, it is assigned the value of the first one of
** the "checkin" or "ci" CGI parameters which is set.
**
** If a parameter is not NULL, it will be assigned NULL if the
** corresponding parameter is not set.
**
** Returns the number of non-NULL values it assigns to arguments. Thus
** if passed (&x, NULL), it returns 1 if it assigns non-NULL to *x and
** 0 if it assigns NULL to *x.
*/
static int fileedit_get_fnci_args( const char **zFn, const char **zCi ){
  int rc = 0;
  if(zCi!=0){
    *zCi = PD("checkin",P("ci"));
    if( *zCi ) ++rc;
  }
  if(zFn!=0){
    *zFn = PD("filename",P("fn"));
    if (*zFn) ++rc;
  }
  return rc;
}

/*
** Passed the values of the "checkin" and "filename" request
** properties, this function verifies that they are valid and
** populates:
**
** - *zRevUuid = the fully-expanded value of zRev (owned by the
**    caller). zRevUuid may be NULL.
**
** - *vid = the RID of zRevUuid. May not be NULL.
**
** - *frid = the RID of zFilename's blob content. May not be NULL
**   unless zFilename is also NULL. If BOTH of zFilename and frid are
**   NULL then no confirmation is done on the filename argument - only
**   zRev is checked.
**
** Returns 0 if the given file is not in the given checkin or if
** fileedit_ajax_check_filename() fails, else returns true.  If it
** returns false, it queues up an error response and the caller must
** return immediately.
*/
static int fileedit_ajax_setup_filerev(const char * zRev,
                                       char ** zRevUuid,
                                       int * vid,
                                       const char * zFilename,
                                       int * frid){
  char * zFileUuid;    /* file UUID */
  const int checkFile = zFilename!=0 || frid!=0;
  
  if(checkFile && !fileedit_ajax_check_filename(zFilename)){
    return 0;
  }
  *vid = symbolic_name_to_rid(zRev, "ci");
  if(0==*vid){
    fileedit_ajax_error(404,"Cannot resolve name as a checkin: %s",
                        zRev);
    return 0;
  }else if(*vid<0){
    fileedit_ajax_error(400,"Checkin name is ambiguous: %s",
                        zRev);
    return 0;
  }
  if(checkFile){
    zFileUuid = fileedit_file_uuid(zFilename, *vid, 0);
    if(zFileUuid==0){
      fileedit_ajax_error(404,"Checkin does not contain file.");
      return 0;
    }
  }
  if(zRevUuid!=0){
    *zRevUuid = rid_to_uuid(*vid);
  }
  if(checkFile){
    assert(zFileUuid!=0);
    if(frid!=0){
      *frid = fast_uuid_to_rid(zFileUuid);
    }
    fossil_free(zFileUuid);
  }
  return 1;
}

/*
** AJAX route /fileedit?ajax=content
**
** Query parameters:
**
** filename=FILENAME
** checkin=CHECKIN_NAME
**
** User must have Write access to use this page.
**
** Responds with the raw content of the given page. On error it
** produces a JSON response as documented for fileedit_ajax_error().
**
** Extra response headers:
**
** x-fileedit-file-perm: empty or "x" or "l", representing PERM_REG,
** PERM_EXE, or PERM_LINK, respectively.
**
** x-fileedit-checkin-branch: branch name for the passed-in checkin.
*/
static void fileedit_ajax_content(void){
  const char * zFilename = 0;
  const char * zRev = 0;
  int vid, frid;
  Blob content = empty_blob;
  const char * zMime;

  fileedit_get_fnci_args( &zFilename, &zRev );
  if(!fileedit_ajax_boostrap(0)
     || !fileedit_ajax_setup_filerev(zRev, 0, &vid,
                                     zFilename, &frid)){
    return;
  }
  zMime = mimetype_from_name(zFilename);
  content_get(frid, &content);
  if(0==zMime){
    if(looks_like_binary(&content)){
      zMime = "application/octet-stream";
    }else{
      zMime = "text/plain";
    }
  }
  { /* Send the is-exec bit via response header so that the UI can be
    ** updated to account for that. */
    int fperm = 0;
    char * zFuuid = fileedit_file_uuid(zFilename, vid, &fperm);
    const char * zPerm = mfile_permint_mstring(fperm);
    assert(zFuuid);
    cgi_printf_header("x-fileedit-file-perm:%s\r\n", zPerm);
    fossil_free(zFuuid);
  }
  { /* Send branch name via response header for UI usability reasons */
    char * zBranch = branch_of_rid(vid);
    if(zBranch!=0 && zBranch[0]!=0){
      cgi_printf_header("x-fileedit-checkin-branch: %s\r\n", zBranch);
    }
    fossil_free(zBranch);
  }
  cgi_set_content_type(zMime);
  cgi_set_content(&content);
}

/*
** AJAX route /fileedit?ajax=preview
**
** Required query parameters:
**
** filename=FILENAME
** content=text
**
** Optional query parameters:
**
** render_mode=integer (FE_RENDER_xxx) (default=FE_RENDER_GUESS)
**
** ln=0 or 1 to disable/enable line number mode in
** FE_RENDER_PLAIN_TEXT mode.
**
** iframe_height=integer (default=40) Height, in EMs of HTML preview
** iframe.
**
** User must have Write access to use this page.
**
** Responds with the HTML content of the preview. On error it produces
** a JSON response as documented for fileedit_ajax_error().
**
** Extra response headers:
**
** x-fileedit-render-mode: string representing the rendering mode
** which was really used (which will differ from the requested mode
** only if mode 0 (guess) was requested). The names are documented
** below in code and match those in the emitted JS object
** fossil.page.previewModes.
*/
static void fileedit_ajax_preview(void){
  const char * zFilename = 0;
  const char * zContent = P("content");
  int renderMode = atoi(PD("render_mode","0"));
  int ln = atoi(PD("ln","0"));
  int iframeHeight = atoi(PD("iframe_height","40"));
  Blob content = empty_blob;
  const char * zRenderMode = 0;
  fileedit_get_fnci_args( &zFilename, 0 );
  if(!fileedit_ajax_boostrap(1)
     || !fileedit_ajax_check_filename(zFilename)){
    return;
  }
  cgi_set_content_type("text/html");
  blob_init(&content, zContent, -1);
  fileedit_render_preview(&content, zFilename,
                          ln ? FE_PREVIEW_LINE_NUMBERS : 0,
                          &renderMode, iframeHeight);
  /*
  ** Now tell the caller if we did indeed use FE_RENDER_WIKI, so that
  ** they can re-set the <base href> to an appropriate value (which
  ** requires knowing the content's current checkin version, which we
  ** don't have here).
  */
  switch(renderMode){
    /* The strings used here MUST correspond to those used in the JS-side
    ** fossil.page.previewModes map.
    */
    case FE_RENDER_WIKI: zRenderMode = "wiki"; break;
    case FE_RENDER_HTML_INLINE: zRenderMode = "htmlInline"; break;
    case FE_RENDER_HTML_IFRAME: zRenderMode = "htmlIframe"; break;
    case FE_RENDER_PLAIN_TEXT: zRenderMode = "text"; break;
    case FE_RENDER_GUESS:
      assert(!"cannot happen");
  }
  if(zRenderMode!=0){
    cgi_printf_header("x-fileedit-render-mode: %s\r\n", zRenderMode);
  }
}

/*
** AJAX route /fileedit?ajax=diff
**
** Required query parameters:
**
** filename=FILENAME
** content=text
** checkin=checkin version
**
** Optional parameters:
**
** sbs=integer (1=side-by-side or 0=unified, default=0)
**
** ws=integer (0=diff whitespace, 1=ignore EOL ws, 2=ignore all ws)
**
** Reminder to self: search info.c for isPatch to see how a
** patch-style siff can be produced.
**
** User must have Write access to use this page.
**
** Responds with the HTML content of the diff. On error it produces a
** JSON response as documented for fileedit_ajax_error().
*/
static void fileedit_ajax_diff(void){
  /*
  ** Reminder: we only need the filename to perform valdiation
  ** against fileedit_is_editable(), else this route could be
  ** abused to get diffs against content disallowed by the
  ** whitelist.
  */
  const char * zFilename = 0;
  const char * zRev = 0;
  const char * zContent = P("content");
  char * zRevUuid = 0;
  int vid, frid, iFlag;
  u64 diffFlags = DIFF_HTML | DIFF_NOTTOOBIG;
  Blob content = empty_blob;

  iFlag = atoi(PD("sbs","0"));
  if(0==iFlag){
    diffFlags |= DIFF_LINENO;
  }else{
    diffFlags |= DIFF_SIDEBYSIDE;
  }
  iFlag = atoi(PD("ws","2"));
  if(2==iFlag){
    diffFlags |= DIFF_IGNORE_ALLWS;
  }else if(1==iFlag){
    diffFlags |= DIFF_IGNORE_EOLWS;
  }
  diffFlags |= DIFF_STRIP_EOLCR;
  fileedit_get_fnci_args( &zFilename, &zRev );
  if(!fileedit_ajax_boostrap(1)
     || !fileedit_ajax_setup_filerev(zRev, &zRevUuid, &vid,
                                     zFilename, &frid)){
    return;
  }
  if(!zContent){
    zContent = "";
  }
  cgi_set_content_type("text/html");
  blob_init(&content, zContent, -1);
  fileedit_render_diff(&content, frid, zRevUuid, diffFlags);
  fossil_free(zRevUuid);
  blob_reset(&content);
}

/*
** Sets up and validates most, but not all, of p's checkin-related
** state from the CGI environment. Returns 0 on success or a suggested
** HTTP result code on error, in which case a message will have been
** written to pErr.
**
** It always fails if it cannot completely resolve the 'file' and 'r'
** parameters, including verifying that the refer to a real
** file/version combination and editable by the current user. All
** others are optional (at this level, anyway, but upstream code might
** require them).
**
** If the 3rd argument is not NULL and an error is related to a
** missing arg then *bIsMissingArg is set to true. This is
** intended to allow /fileedit to squelch certain initialization
** errors.
**
** Intended to be used only by /filepage and /filepage_commit.
*/
static int fileedit_setup_cimi_from_p(CheckinMiniInfo * p, Blob * pErr,
                                      int * bIsMissingArg){
  char * zFileUuid = 0;          /* UUID of file content */
  const char * zFlag;            /* generic flag */
  int rc = 0, vid = 0, frid = 0; /* result code, checkin/file rids */ 

#define fail(EXPR) blob_appendf EXPR; goto end_fail
  zFlag = PD("filename",P("fn"));
  if(zFlag==0 || !*zFlag){
    rc = 400;
    if(bIsMissingArg){
      *bIsMissingArg = 1;
    }
    fail((pErr,"Missing required 'filename' parameter."));
  }
  p->zFilename = mprintf("%s",zFlag);

  if(0==fileedit_is_editable(p->zFilename)){
    rc = 403;
    fail((pErr,"Filename [%h] is disallowed "
          "by the [fileedit-glob] repository "
          "setting.",
          p->zFilename));
  }

  zFlag = PD("checkin",P("ci"));
  if(!zFlag){
    rc = 400;
    if(bIsMissingArg){
      *bIsMissingArg = 1;
    }
    fail((pErr,"Missing required 'checkin' parameter."));
  }
  vid = symbolic_name_to_rid(zFlag, "ci");
  if(0==vid){
    rc = 404;
    fail((pErr,"Could not resolve checkin version."));
  }else if(vid<0){
    rc = 400;
    fail((pErr,"Checkin name is ambiguous."));
  }
  p->zParentUuid = rid_to_uuid(vid)/*fully expand it*/;

  zFileUuid = fileedit_file_uuid(p->zFilename, vid, &p->filePerm);
  if(!zFileUuid){
    rc = 404;
    fail((pErr,"Checkin [%S] does not contain file: "
          "[%h]", p->zParentUuid, p->zFilename));
  }else if(PERM_LNK==p->filePerm){
    rc = 400;
    fail((pErr,"Editing symlinks is not permitted."));
  }

  /* Find the repo-side file entry or fail... */
  frid = fast_uuid_to_rid(zFileUuid);
  assert(frid);

  /* Read file content from submit request or repo... */
  zFlag = P("content");
  if(zFlag==0){
    content_get(frid, &p->fileContent);
  }else{
    blob_init(&p->fileContent,zFlag,-1);
  }
  if(looks_like_binary(&p->fileContent)){
    rc = 400;
    fail((pErr,"File appears to be binary. Cannot edit: "
          "[%h]",p->zFilename));
  }

  zFlag = PT("comment");
  if(zFlag!=0 && *zFlag!=0){
    blob_append(&p->comment, zFlag, -1);
  }
  zFlag = P("comment_mimetype");
  if(zFlag){
    p->zCommentMimetype = mprintf("%s",zFlag);
    zFlag = 0;
  }
#define p_int(K) atoi(PD(K,"0"))
  if(p_int("dry_run")!=0){
    p->flags |= CIMINI_DRY_RUN;
  }
  if(p_int("allow_fork")!=0){
    p->flags |= CIMINI_ALLOW_FORK;
  }
  if(p_int("allow_older")!=0){
    p->flags |= CIMINI_ALLOW_OLDER;
  }
  if(0==p_int("exec_bit")){
    p->filePerm = PERM_REG;
  }else{
    p->filePerm = PERM_EXE;
  }
  if(p_int("allow_merge_conflict")!=0){
    p->flags |= CIMINI_ALLOW_MERGE_MARKER;
  }
  if(p_int("prefer_delta")!=0){
    p->flags |= CIMINI_PREFER_DELTA;
  }

  /* EOL conversion policy... */
  switch(p_int("eol")){
    case 1: p->flags |= CIMINI_CONVERT_EOL_UNIX; break;
    case 2: p->flags |= CIMINI_CONVERT_EOL_WINDOWS; break;
    default: p->flags |= CIMINI_CONVERT_EOL_INHERIT; break;
  }
#undef p_int
  /*
  ** TODO?: date-override date selection field. Maybe use
  ** an input[type=datetime-local].
  */
  p->zUser = mprintf("%s",g.zLogin);
  return 0;
end_fail:
#undef fail
  fossil_free(zFileUuid);
  return rc ? rc : 500;
}

/*
** AJAX route /fileedit?ajax=filelist
**
** Fetches a JSON-format list of leaves and/or filenames for use in
** creating a file selection list in /fileedit. It has different modes
** of operation depending on its arguments:
**
** 'leaves': just fetch a list of open leaf versions, in this
** format:
**
** [
**   {checkin: UUID, branch: branchName, timestamp: string}
** ]
**
** The entries are ordered newest first.
**
** 'checkin=CHECKIN_NAME': fetch the current list of is-editable files
** for the current user and given checkin name:
**
** {
**   checkin: UUID,
**   editableFiles: [ filename1, ... filenameN ] // sorted by name
** }
**
** On error it produces a JSON response as documented for
** fileedit_ajax_error().
*/
static void fileedit_ajax_filelist(void){
  const char * zCi = PD("checkin",P("ci"));
  Blob sql = empty_blob;
  Stmt q = empty_Stmt;
  int i = 0;

  if(!fileedit_ajax_boostrap(0)){
    return;
  }
  cgi_set_content_type("application/json");
  if(zCi!=0){
    char * zCiFull = 0;
    int vid = 0;
    if(0==fileedit_ajax_setup_filerev(zCi, &zCiFull, &vid, 0, 0)){
      /* Error already reported */
      return;
    }
    CX("{\"checkin\":%!j,"
       "\"editableFiles\":[", zCiFull);
    blob_append_sql(&sql, "SELECT filename FROM files_of_checkin(%Q) "
                    "ORDER BY filename %s",
                    zCiFull, filename_collation());
    db_prepare_blob(&q, &sql);
    while( SQLITE_ROW==db_step(&q) ){
      const char * zFilename = db_column_text(&q, 0);
      if(fileedit_is_editable(zFilename)){
        if(i++){
          CX(",");
        }
        CX("%!j", zFilename);
      }
    }
    db_finalize(&q);
    CX("]}");
  }else if(P("leaves")!=0){
    blob_append(&sql, timeline_query_for_tty(), -1);
    blob_append_sql(&sql, " AND blob.rid IN (SElECT rid FROM leaf "
                    "WHERE NOT EXISTS("
                    "SELECT 1 from tagxref WHERE tagid=%d AND "
                    "tagtype>0 AND rid=leaf.rid"
                    ")) "
                    "ORDER BY mtime DESC", TAG_CLOSED);
    db_prepare_blob(&q, &sql);
    CX("[");
    while( SQLITE_ROW==db_step(&q) ){
      if(i++){
        CX(",");
      }
      CX("{");
      CX("\"checkin\":%!j,", db_column_text(&q, 1));
      CX("\"branch\":%!j,", db_column_text(&q, 7));
      CX("\"timestamp\":%!j", db_column_text(&q, 2));
      CX("}");
    }
    CX("]");
    db_finalize(&q);
  }else{
    fileedit_ajax_error(500, "Unhandled URL argument.");
  }
}

/*
** AJAX route /fileedit?ajax=commit
**
** Required query parameters:
** 
** filename=FILENAME
** checkin=Parent checkin UUID
** content=text
** comment=non-empty text
**
** Optional query parameters:
**
** comment_mimetype=text (NOT currently honored)
**
** dry_run=int (1 or 0)
**
** include_manifest=int (1 or 0), whether to include
** the generated manifest in the response.
** 
**
** User must have Write permissions to use this page.
**
** Responds with JSON (with some state repeated
** from the input in order to avoid certain race conditions
** client-side):
**
** {
**  checkin: newUUID,
**  filename: theFilename,
**  mimetype: string,
**  branch: name of the checkin's branch,
**  isExe: bool,
**  dryRun: bool,
**  manifest: text of manifest,
** }
**
** On error it produces a JSON response as documented for
** fileedit_ajax_error().
*/
static void fileedit_ajax_commit(void){
  Blob err = empty_blob;      /* Error messages */
  Blob manifest = empty_blob; /* raw new manifest */
  CheckinMiniInfo cimi;       /* checkin state */
  int rc;                     /* generic result code */
  int newVid = 0;             /* new version's RID */
  char * zNewUuid = 0;        /* newVid's UUID */
  char const * zMimetype;
  char * zBranch = 0;

  if(!fileedit_ajax_boostrap(1)){
    return;
  }
  db_begin_transaction();
  CheckinMiniInfo_init(&cimi);
  rc = fileedit_setup_cimi_from_p(&cimi, &err, 0);
  if(0!=rc){
    fileedit_ajax_error(rc,"%b",&err);
    goto end_cleanup;
  }
  if(blob_size(&cimi.comment)==0){
    fileedit_ajax_error(400,"Empty checkin comment is not permitted.");
    goto end_cleanup;
  }
  if(0!=atoi(PD("include_manifest","0"))){
    cimi.pMfOut = &manifest;
  }
  checkin_mini(&cimi, &newVid, &err);
  if(blob_size(&err)){
    fileedit_ajax_error(500,"%b",&err);
    goto end_cleanup;
  }
  assert(newVid>0);
  zNewUuid = rid_to_uuid(newVid);
  cgi_set_content_type("application/json");
  CX("{");
  CX("\"checkin\":%!j,", zNewUuid);
  CX("\"filename\":%!j,", cimi.zFilename);
  CX("\"isExe\": %s,", cimi.filePerm==PERM_EXE ? "true" : "false");
  zMimetype = mimetype_from_name(cimi.zFilename);
  if(zMimetype!=0){
    CX("\"mimetype\": %!j,", zMimetype);
  }
  zBranch = branch_of_rid(newVid);
  if(zBranch!=0){
    CX("\"branch\": %!j,", zBranch);
    fossil_free(zBranch);
  }
  CX("\"dryRun\": %s",
     (CIMINI_DRY_RUN & cimi.flags) ? "true" : "false");
  if(blob_size(&manifest)>0){
    CX(",\"manifest\": %!j", blob_str(&manifest));
  }
  CX("}");
end_cleanup:
  db_end_transaction(0/*noting that dry-run mode will have already
                      ** set this to rollback mode. */);
  fossil_free(zNewUuid);
  blob_reset(&err);
  blob_reset(&manifest);
  CheckinMiniInfo_cleanup(&cimi);
}

/*
** WEBPAGE: fileedit
**
** Enables the online editing and committing of individual text files.
** Requires that the user have Write permissions.
**
** Optional query parameters:
**
**    filename=FILENAME   Repo-relative path to the file.
**    checkin=VERSION     Checkin version, using any unambiguous
**                        supported symbolic version name.
**
** Internal-use parameters:
**
**    name=string         The name of a page-specific AJAX operation.
**
** Noting that fossil internally stores all URL path components after
** the first as the "name" value. Thus /fileedit?name=blah is
** equivalent to /fileedit/blah. The latter is the preferred
** form. This means, however, that no fileedit ajax routes may make
** use of the name parameter.
**
** Which additional parameters are used by each distinct ajax value is
** an internal implementation detail and may change with any given
** build of this code. An unknown "name" value triggers an error, as
** documented for fileedit_ajax_error().
*/
void fileedit_page(void){
  const char * zFilename = 0;          /* filename. We'll accept 'name'
                                           because that param is handled
                                           specially by the core. */
  const char * zRev = 0;                /* checkin version */
  const char * zFileMime = 0;           /* File mime type guess */
  CheckinMiniInfo cimi;                 /* Checkin state */
  int previewHtmlHeight = 0;            /* iframe height (EMs) */
  int previewRenderMode = FE_RENDER_GUESS; /* preview mode */
  Blob err = empty_blob;                /* Error report */
  Blob endScript = empty_blob;          /* Script code to run at the
                                           end. This content will be
                                           combined into a single JS
                                           function call, thus each
                                           entry must end with a
                                           semicolon. */
  const char *zAjax = P("name");        /* Name of AJAX route for
                                           sub-dispatching. */

  /* Allow no access to this page without check-in privilege */
  login_check_credentials();
  if( !g.perm.Write ){
    if(zAjax!=0){
      fileedit_ajax_error(403, "Write permissions required.");
    }else{
      login_needed(g.anon.Write);
    }
    return;
  }
  /* No access to anything on this page if the fileedit-glob is empty */
  if( fileedit_glob()==0 ){
    if(zAjax!=0){
      fileedit_ajax_error(403, "Online editing is disabled for this "
                          "repository.");
      return;
    }
    style_header("File Editor (disabled)");
    CX("<h1>Online File Editing Is Disabled</h1>\n");
    if( g.perm.Admin ){
      CX("<p>To enable online editing, the "
         "<a href='%R/setup_settings'>"
         "<code>fileedit-glob</code> repository setting</a>\n"
         "must be set to a comma- and/or newine-delimited list of glob\n"
         "values matching files which may be edited online."
         "</p>\n");
    }else{
      CX("<p>Online editing is disabled for this repository.</p>\n");
    }
    style_footer();
    return;
  }

  /* Dispatch AJAX methods based tail of the request URI.
  ** The AJAX parts do their own permissions/CSRF check and
  ** fail with a JSON-format response if needed.
  */
  if( 0!=zAjax ){
    if(0==strcmp("content",zAjax)){
      fileedit_ajax_content();
    }else if(0==strcmp("preview",zAjax)){
      fileedit_ajax_preview();
    }else if(0==strcmp("filelist",zAjax)){
      fileedit_ajax_filelist();
    }else if(0==strcmp("diff",zAjax)){
      fileedit_ajax_diff();
    }else if(0==strcmp("commit",zAjax)){
      fileedit_ajax_commit();
    }else{
      fileedit_ajax_error(500, "Unhandled ajax route name.");
    }
    return;
  }

  db_begin_transaction();
  CheckinMiniInfo_init(&cimi);
  style_header("File Editor");
  /* As of this point, don't use return or fossil_fatal(). Write any
  ** error in (&err) and goto end_footer instead so that we can be
  ** sure to emit the error message, do any cleanup, and end the
  ** transaction cleanly.
  */
  {
    int isMissingArg = 0;
    if(fileedit_setup_cimi_from_p(&cimi, &err, &isMissingArg)==0){
      zFilename = cimi.zFilename;
      zRev = cimi.zParentUuid;
      assert(zRev);
      assert(zFilename);
      zFileMime = mimetype_from_name(cimi.zFilename);
    }else if(isMissingArg!=0){
      /* Squelch these startup warnings - they're non-fatal now but
      ** used to be fatal. */
      blob_reset(&err);
    }
  }

  /********************************************************************
  ** All errors which "could" have happened up to this point are of a
  ** degree which keep us from rendering the rest of the page, and
  ** thus have already caused us to skipped to the end of the page to
  ** render the errors. Any up-coming errors, barring malloc failure
  ** or similar, are not "that" fatal. We can/should continue
  ** rendering the page, then output the error message at the end.
  ********************************************************************/

  {
    /* The CSS for this page lives in a common file but much of it we
    ** don't want inadvertently being used by other pages. We don't
    ** have a common, page-specific container we can filter our CSS
    ** selectors, but we do have the BODY, which we can decorate with
    ** whatever CSS we wish...
    */
    style_emit_script_tag(0,0);
    CX("document.body.classList.add('fileedit');\n");
    style_emit_script_tag(1,0);
  }
  
  /* Status bar */
  CX("<div id='fossil-status-bar' "
     "title='Status message area. Double-click to clear them.'>"
     "Status messages will go here.</div>\n"
     /* will be moved into the tab container via JS */);

  /* Main tab container... */
  CX("<div id='fileedit-tabs' class='tab-container'></div>");

  /***** File/version info tab *****/
  {
    CX("<div id='fileedit-tab-fileselect' "
       "data-tab-parent='fileedit-tabs' "
       "data-tab-label='File Info &amp; Selection'"
       ">");
    CX("<fieldset id='file-version-details'>"
       "<legend>File/Version</legend>"
       "<div>No file loaded.</div>"
       "</fieldset>");
    CX("<h1>Select a file to edit:</h1>");
    CX("<div id='fileedit-file-selector'></div>");
    CX("</div>"/*#fileedit-tab-fileselect*/);
  }
  
  /******* Content tab *******/
  {
    CX("<div id='fileedit-tab-content' "
       "data-tab-parent='fileedit-tabs' "
       "data-tab-label='File Content'"
       ">");
    CX("<div class='flex-container flex-row child-gap-small'>");
    CX("<button class='fileedit-content-reload confirmer' "
       "title='Reload the file from the server, discarding "
       "any local edits. To help avoid accidental loss of "
       "edits, it requires confirmation (a second click) within "
       "a few seconds or it will not reload.'"
       ">Discard &amp; Reload</button>");
    style_select_list_int("select-font-size",
                          "editor_font_size", "Editor font size",
                          NULL/*tooltip*/,
                          100,
                          "100%", 100, "125%", 125,
                          "150%", 150, "175%", 175,
                          "200%", 200, NULL);
    CX("</div>");
    CX("<div class='flex-container flex-column stretch'>");
    CX("<textarea name='content' id='fileedit-content-editor' "
       "class='fileedit' "
       "rows='20' cols='80'>");
    CX("</textarea>");
    CX("</div>"/*textarea wrapper*/);
    CX("</div>"/*#tab-file-content*/);
  }

  /****** Preview tab ******/
  {
    CX("<div id='fileedit-tab-preview' "
       "data-tab-parent='fileedit-tabs' "
       "data-tab-label='Preview'"
       ">");
    CX("<div class='fileedit-options flex-container flex-row'>");
    CX("<button id='btn-preview-refresh' "
       "data-f-preview-from='fileContent' "
       /* ^^^ fossil.page[methodName]() OR text source elem ID,
      ** but we need a method in order to support clients swapping out
      ** the text editor with their own. */
       "data-f-preview-via='_postPreview' "
       /* ^^^ fossil.page[methodName](content, callback) */
       "data-f-preview-to='#fileedit-tab-preview-wrapper' "
       /* ^^^ dest elem ID */
       ">Refresh</button>");
    /* Toggle auto-update of preview when the Preview tab is selected. */
    style_labeled_checkbox("cb-preview-autoupdate",
                           NULL,
                           "Auto-refresh?",
                           "1", 1,
                           "If on, the preview will automatically "
                           "refresh when this tab is selected.");

    /* Default preview rendering mode selection... */
    previewRenderMode = zFileMime
      ? fileedit_render_mode_for_mimetype(zFileMime)
      : FE_RENDER_GUESS;
    style_select_list_int("select-preview-mode",
                          "preview_render_mode",
                          "Preview Mode",
                          "Preview mode format.",
                          previewRenderMode,
                          "Guess", FE_RENDER_GUESS,
                          "Wiki/Markdown", FE_RENDER_WIKI,
                          "HTML (iframe)", FE_RENDER_HTML_IFRAME,
                          "HTML (inline)", FE_RENDER_HTML_INLINE,
                          "Plain Text", FE_RENDER_PLAIN_TEXT,
                          NULL);
    /*
    ** Set up a JS-side mapping of the FE_RENDER_xyz values. This is
    ** used for dynamically toggling certain UI components on and off.
    */
    blob_appendf(&endScript, "fossil.page.previewModes={"
                 "guess: %d, %d: 'guess', wiki: %d, %d: 'wiki',"
                 "htmlIframe: %d, %d: 'htmlIframe', "
                 "htmlInline: %d, %d: 'htmlInline', "
                 "text: %d, %d: 'text'"
                 "};\n",
                 FE_RENDER_GUESS, FE_RENDER_GUESS,
                 FE_RENDER_WIKI, FE_RENDER_WIKI,
                 FE_RENDER_HTML_IFRAME, FE_RENDER_HTML_IFRAME,
                 FE_RENDER_HTML_INLINE, FE_RENDER_HTML_INLINE,
                 FE_RENDER_PLAIN_TEXT, FE_RENDER_PLAIN_TEXT);
    /* Allow selection of HTML preview iframe height */
    previewHtmlHeight = 40;
    style_select_list_int("select-preview-html-ems",
                          "preview_html_ems",
                          "HTML Preview IFrame Height (EMs)",
                          "Height (in EMs) of the iframe used for "
                          "HTML preview",
                          previewHtmlHeight,
                          "", 20, "", 40,
                          "", 60, "", 80,
                          "", 100, NULL);
    /* Selection of line numbers for text preview */
    style_labeled_checkbox("cb-line-numbers",
                           "preview_ln",
                           "Add line numbers to plain-text previews?",
                           "1", P("preview_ln")!=0,
                           "If on, plain-text files (only) will get "
                           "line numbers added to the preview.");
    CX("</div>"/*.fileedit-options*/);
    CX("<div id='fileedit-tab-preview-wrapper'></div>");
    CX("</div>"/*#fileedit-tab-preview*/);
  }

  /****** Diff tab ******/
  {
    CX("<div id='fileedit-tab-diff' "
       "data-tab-parent='fileedit-tabs' "
       "data-tab-label='Diff'"
       ">");

    CX("<div class='fileedit-options flex-container flex-row' "
       "id='fileedit-tab-diff-buttons'>");
    CX("<button class='sbs'>Side-by-side</button>"
       "<button class='unified'>Unified</button>");
    if(0){
      /* For the time being let's just ignore all whitespace
      ** changes, as files with Windows-style EOLs always show
      ** more diffs than we want then they're submitted to
      ** ?ajax=diff because JS normalizes them to Unix EOLs.
      ** We can revisit this decision later. */
      style_select_list_int("diff-ws-policy",
                            "diff_ws", "Whitespace",
                            "Whitespace handling policy.",
                            2,
                            "Diff all whitespace", 0,
                            "Ignore EOL whitespace", 1,
                            "Ignore all whitespace", 2,
                            NULL);
    }
    CX("</div>");
    CX("<div id='fileedit-tab-diff-wrapper'>"
       "Diffs will be shown here."
       "</div>");
    CX("</div>"/*#fileedit-tab-diff*/);
  }

  /****** Commit ******/
  CX("<div id='fileedit-tab-commit' "
     "data-tab-parent='fileedit-tabs' "
     "data-tab-label='Commit'"
     ">");
  {
    /******* Commit flags/options *******/
    CX("<div class='fileedit-options flex-container flex-row'>");
    style_labeled_checkbox("cb-dry-run",
                           "dry_run", "Dry-run?", "1",
                           0,
                           "In dry-run mode, the Commit button performs"
                           "all work needed for committing changes but "
                           "then rolls back the transaction, and thus "
                           "does not really commit.");
    style_labeled_checkbox("cb-allow-fork",
                           "allow_fork", "Allow fork?", "1",
                           cimi.flags & CIMINI_ALLOW_FORK,
                           "Allow committing to create a fork?");
    style_labeled_checkbox("cb-allow-older",
                           "allow_older", "Allow older?", "1",
                           cimi.flags & CIMINI_ALLOW_OLDER,
                           "Allow saving against a parent version "
                           "which has a newer timestamp?");
    style_labeled_checkbox("cb-exec-bit",
                           "exec_bit", "Executable?", "1",
                           PERM_EXE==cimi.filePerm,
                           "Set the executable bit?");
    style_labeled_checkbox("cb-allow-merge-conflict",
                           "allow_merge_conflict",
                           "Allow merge conflict markers?", "1",
                           cimi.flags & CIMINI_ALLOW_MERGE_MARKER,
                           "Allow saving even if the content contains "
                           "what appear to be fossil merge conflict "
                           "markers?");
    style_labeled_checkbox("cb-prefer-delta",
                           "prefer_delta",
                           "Prefer delta manifest?", "1",
                           db_get_boolean("forbid-delta-manifests",0)
                           ? 0
                           : (db_get_boolean("seen-delta-manifest",0)
                              || cimi.flags & CIMINI_PREFER_DELTA),
                           "Will create a delta manifest, instead of "
                           "baseline, if conditions are favorable to "
                           "do so. This option is only a suggestion.");
    style_labeled_checkbox("cb-include-manifest",
                           "include_manifest",
                           "Response manifest?", "1",
                           0,
                           "Include the manifest in the response? "
                           "It's generally only useful for debug "
                           "purposes.");
    style_select_list_int("select-eol-style",
                          "eol", "EOL Style",
                          "EOL conversion policy, noting that "
                          "webpage-side processing may implicitly change "
                          "the line endings of the input.",
                          (cimi.flags & CIMINI_CONVERT_EOL_UNIX)
                          ? 1 : (cimi.flags & CIMINI_CONVERT_EOL_WINDOWS
                                 ? 2 : 0),
                          "Inherit", 0,
                          "Unix", 1,
                          "Windows", 2,
                          NULL);

    CX("</div>"/*checkboxes*/);
  }

  { /******* Commit comment, button, and result manifest *******/
    CX("<fieldset class='fileedit-options commit-message'>"
       "<legend>Message (required)</legend><div>\n");
    /* We have two comment input fields, defaulting to single-line
    ** mode. JS code sets up the ability to toggle between single-
    ** and multi-line modes. */
    CX("<input type='text' name='comment' "
       "id='fileedit-comment'></input>");
    CX("<textarea name='commentBig' class='hidden' "
       "rows='5' id='fileedit-comment-big'></textarea>\n");
    { /* comment options... */
      CX("<div class='flex-container flex-column child-gap-small'>");
      CX("<button id='comment-toggle' "
         "title='Toggle between single- and multi-line comment mode, "
         "noting that switching from multi- to single-line will cause "
         "newlines to get stripped.'"
         ">Toggle single-/multi-line</button> ");
      if(0){
        /* Manifests support an N-card (comment mime type) but it has
        ** yet to be honored where comments are rendered, so we don't
        ** currently offer it as an option here:
        ** https://fossil-scm.org/forum/forumpost/662da045a1
        **
        ** If/when it's ever implemented, simply enable this block and
        ** adjust the container's layout accordingly (as of this
        ** writing, that means changing the CSS class from
        ** 'flex-container flex-column' to 'flex-container flex-row').
        */
        style_select_list_str("comment-mimetype", "comment_mimetype",
                              "Comment style:",
                              "Specify how fossil will interpret the "
                              "comment string.",
                              NULL,
                              "Fossil", "text/x-fossil-wiki",
                              "Markdown", "text/x-markdown", 
                              "Plain text", "text/plain",
                              NULL);
        CX("</div>\n");
      }
      CX("<div class='fileedit-hint flex-container flex-row'>"
         "(Warning: switching from multi- to single-line mode will "
         "strip out all newlines!)</div>");
    }
    CX("</div></fieldset>\n"/*commit comment options*/);
    CX("<div class='flex-container flex-column' "
       "id='fileedit-commit-button-wrapper'>"
       "<button id='fileedit-btn-commit'>Commit</button>"
       "</div>\n");
    CX("<div id='fileedit-manifest'></div>\n"
       /* Manifest gets rendered here after a commit. */);
  }
  CX("</div>"/*#fileedit-tab-commit*/);

  /****** Help/Tips ******/
  CX("<div id='fileedit-tab-help' "
     "data-tab-parent='fileedit-tabs' "
     "data-tab-label='Help'"
     ">");
  {
    CX("<h1>Help &amp; Tips</h1>");
    CX("<ul>");
    CX("<li><strong>Only files matching the <code>fileedit-glob</code> "
       "repository setting</strong> can be edited online. That setting "
       "must be a comma- or newline-delimited list of glob patterns "
       "for files which may be edited online.</li>");
    CX("<li>Committing edits creates a new commit record with a single "
       "modified file.</li>");
    CX("<li>\"Delta manifests\" (see the checkbox on the Commit tab) "
       "make for smaller commit records, especially in repositories "
       "with many files.</li>");
    CX("<li>The file selector allows, for usability's sake, only files "
       "in leaf check-ins to be selected, but files may be edited via "
       "non-leaf check-ins by passing them as the <code>filename</code> "
       "and <code>checkin</code> URL arguments to this page.</li>");
    CX("<li>The editor stores some number of local edits in one of "
       "<code>window.fileStorage</code> or "
       "<code>window.sessionStorage</code>, if able, but which storage "
       "is unspecified and may differ across environments. When "
       "committing or force-reloading a file, local edits to that "
       "file/check-in combination are discarded.</li>");
    CX("</ul>");
  }
  CX("</div>"/*#fileedit-tab-help*/);

  {
    /* Dynamically populate the editor, display any error in the err
    ** blob, and/or switch to tab #0, where the file selector
    ** lives... */
    blob_appendf(&endScript,
                 "window.addEventListener('load',");
    if(zRev && zFilename){
      assert(0==blob_size(&err));
      blob_appendf(&endScript,
                   "()=>fossil.page.loadFile(%!j,%!j)",
                   zFilename, cimi.zParentUuid);
    }else{
      blob_appendf(&endScript,"function(){");
      if(blob_size(&err)>0){
        blob_appendf(&endScript,
                     "fossil.error(%!j);\n",
                     blob_str(&err));
      }
      blob_appendf(&endScript,
                   "fossil.page.tabs.switchToTab(0);\n");
      blob_appendf(&endScript,"}");
    }
    blob_appendf(&endScript,", false);\n");
  }

  blob_reset(&err);
  CheckinMiniInfo_cleanup(&cimi);
  style_emit_script_fossil_bootstrap(0);
  append_diff_javascript(1);
  style_emit_script_fetch(0);
  style_emit_script_tabs(0)/*also emits fossil.dom*/;
  style_emit_script_confirmer(0);
  style_emit_script_builtin(0, "fossil.storage.js");
  style_emit_script_builtin(0, "fossil.page.fileedit.js");
  if(blob_size(&endScript)>0){
    style_emit_script_tag(0,0);
    CX("(function(){\n");
    CX("try{\n%b\n}"
       "catch(e){"
       "fossil.error(e);\n"
       "console.error('Exception:',e);\n"
       "}\n",
       &endScript);
    CX("})();");
    style_emit_script_tag(1,0);
  }
  db_end_transaction(0);
  style_footer();
}