/**
   diff-related JS APIs for fossil.
*/
"use strict";
window.fossil.onPageLoad(function(){
  /**
     Adds toggle checkboxes to each file entry in the diff views for
     /info and similar pages.
  */
  const D = window.fossil.dom;
  const addToggle = function(diffElem){
    const sib = diffElem.previousElementSibling,
          btn = sib ? D.addClass(D.checkbox(true), 'diff-toggle') : 0;
    if(!sib) return;
    D.append(sib,btn);
    btn.addEventListener('click', function(){
      diffElem.classList.toggle('hidden');
    }, false);
  };
  document.querySelectorAll('table.diff').forEach(addToggle);
});

window.fossil.onPageLoad(function(){
  const F = window.fossil, D = F.dom;
  const Diff = F.diff = {
    e:{/*certain cached DOM elements*/},
    config: {
      chunkLoadLines: (
        F.config.diffContextLines * 3
        /*per /chat discussion*/
      ) || 20,
      chunkFetch: {
        /* Default callack handlers for Diff.fetchArtifactChunk(),
           unless overridden by options passeed to that function. */
        beforesend: function(){},
        aftersend: function(){},
        onerror: function(e){
          F.toast.error("XHR error: ",e.message);
        }
      }
    }
  };
  /**
     Uses the /jchunk AJAX route to fetch specific lines of a given
     artifact. The argument must be an Object suitable for passing as
     the second argument to fossil.fetch(). Its urlParams property
     must be an object with these properties:

     {
       name: full hash of the target file,
       from: first 1-based line number of the file to fetch (inclusive),
       to: last 1-based line number of the file to fetch (inclusive)
     }

     The fetchOpt object is NOT cloned for use by the call: it is used
     as-is and may be modified by this call. Thus callers "really
     should" pass a temporary object, not a long-lived one.

     If fetchOpt does not define any of the (beforesend, aftersend,
     onerror) callbacks, the defaults from fossil.diff.config.chunkFetch
     are used, so any given client page may override those to provide
     page-level default handling.

     Note that onload callback is ostensibly optional but this
     function is not of much use without an onload
     handler. Conversely, the default onerror handler is often
     customized on a per-page basis to send the error output somewhere
     where the user can see it.

     The response, on success, will be an array of strings, each entry
     being one line from the requested artifact. If the 'to' line is
     greater than the length of the file, the array will be shorter
     than (to-from) lines.

     The /jchunk route reports errors via JSON objects with
     an "error" string property describing the problem.

     This is an async operation. Returns the fossil object.
  */
  Diff.fetchArtifactChunk = function(fetchOpt){
    if(!fetchOpt.beforesend) fetchOpt.beforesend = Diff.config.chunkFetch.beforesend;
    if(!fetchOpt.aftersend) fetchOpt.aftersend = Diff.config.chunkFetch.aftersend;
    if(!fetchOpt.onerror) fetchOpt.onerror = Diff.config.chunkFetch.onerror;
    fetchOpt.responseType = 'json';
    return F.fetch('jchunk', fetchOpt);
  };


  /**
     Extracts either the starting or ending line number from a
     line-numer column in the given tr. isSplit must be true if tr
     represents a split diff, else false. Expects its tr to be valid:
     GIGO applies.  Returns the starting line number if getStart, else
     the ending line number. Returns the line number from the LHS file
     if getLHS is true, else the RHS.
  */
  const extractLineNo = function f(getLHS, getStart, tr, isSplit){
    if(!f.rx){
      f.rx = {
        start: /^\s*(\d+)/,
        end: /(\d+)\n?$/
      }
    }
    const td = tr.querySelector('td:nth-child('+(
      /* TD element with the line numbers */
      getLHS ? 1 : (isSplit ? 4 : 2)
    )+')');
    const m = f.rx[getStart ? 'start' : 'end'].exec(td.innerText);
    return m ? +m[1] : undefined/*"shouldn't happen"*/;
  };

  /**
     Fetches /jchunk for the given TR element then replaces the TR's
     contents with data from the result of that request.
  */
  const fetchTrChunk = function(tr){
    if(tr.dataset.xfer /* already being fetched */) return;
    const table = tr.parentElement.parentElement;
    const hash = table.dataset.lefthash;
    if(!hash) return;
    const isSbs = table.classList.contains('splitdiff')/*else udiff*/;
    tr.dataset.xfer = 1 /* sentinel against multiple concurrent ajax requests */;
    const lineTo = +tr.dataset.endln;
    var lineFrom = +tr.dataset.startln;
    /* TODO: for the time being, for simplicity, we'll read the whole
       [startln, endln] chunk. "Later on" we'll maybe want to read it in
       chunks of, say, 20 lines or so, adjusting lineFrom to be 1 if it would
       be less than 1. */
    Diff.fetchArtifactChunk({
      urlParams:{
        name: hash,
        from: lineFrom,
        to: lineTo
      },
      aftersend: function(){
        delete tr.dataset.xfer;
        Diff.config.chunkFetch.aftersend.apply(
          this, Array.prototype.slice.call(arguments,0)
        );
      },
      onload: function(result){
        //console.debug("Chunk result: ",result);
        /* Replace content of tr.diffskip with the fetches result.
           When we refactor this to load in smaller chunks, we'll instead
           need to keep this skipper in place and:

           - Add a new TR above or above it, as apropriate.

           - Change the TR.dataset.startln/endln values to account for
           the just-fetched set.
         */
        D.clearElement(tr);
        const cols = [], preCode = [D.pre()], preLines = [D.pre(), D.pre()];
        if(isSbs){
          cols.push(D.addClass(D.td(tr), 'diffln', 'difflnl'));
          cols.push(D.addClass(D.td(tr), 'difftxt', 'difftxtl'));
          cols.push(D.addClass(D.td(tr), 'diffsep'));
          cols.push(D.addClass(D.td(tr), 'diffln', 'difflnr'));
          cols.push(D.addClass(D.td(tr), 'difftxt', 'difftxtr'));
          D.append(cols[0], preLines[0]);
          D.append(cols[1], preCode[0]);
          D.append(cols[3], preLines[1]);
          preCode.push(D.pre());
          D.append(cols[4], preCode[1]);
        }else{
          cols.push(D.addClass(D.td(tr), 'diffln', 'difflnl'));
          cols.push(D.addClass(D.td(tr), 'diffln', 'difflnr'));
          cols.push(D.addClass(D.td(tr), 'diffsep'));
          cols.push(D.addClass(D.td(tr), 'difftxt', 'difftxtu'));
          D.append(cols[0], preLines[0]);
          D.append(cols[1], preLines[1]);
          D.append(cols[3], preCode[0]);
        }
        let lineno = [], i;
        for( i = lineFrom; i <= lineTo; ++i ){
          lineno.push(i);
        }
        preLines[0].append(lineno.join('\n')+'\n');
        if(1){
          const code = result.join('\n')+'\n';
          preCode.forEach((e)=>e.innerText = code);
        }
        //console.debug("Updated TR",tr);
        Diff.initTableDiff(table).checkTableWidth(true);
        /*
          Reminders to self during development:

          SBS diff col layout:
            <td.diffln.difflnl><pre>...LHS line numbers...</pre></td>
            <td.difftxt.difftxtl><pre>...code lines...</pre></td>
            <td.diffsep>empty for this case (common lines)</td>
            <td.diffln.difflnr><pre>...RHS line numbers...</pre></td>
            <td.difftxt.difftxtr><pre>...dupe of col 2</pre></td>

          Unified diff col layout:
            <td.diffln.difflnl><pre>LHS line numbers</pre></td>
            <td.diffln.difflnr><pre>RHS line numbers</pre></td>
            <td.diffsep>empty in this case (common lines)</td>
            <td.difftxt.difftxtu><pre>code line</pre></td>

          C-side TODOs:

          - If we have that data readily available, it would be a big
          help (simplify our line calculations) if we stored the line
          number ranges in all elements which have that state handy.
         */
      }
    });
  };
  
  /**
     Installs chunk-loading controls into TR.diffskip element tr.
     Each instance corresponds to a single TR.diffskip element.

     The goal is to base these controls roughly on github's, a good
     example of which, for use as a model, is:

     https://github.com/msteveb/autosetup/commit/235925e914a52a542
  */
  const ChunkLoadControls = function(tr){
    this.e = {/*DOM elements*/
      tr: tr,
      table: tr.parentElement/*TBODY*/.parentElement
    };
    this.isSplit = this.e.table.classList.contains('splitdiff')/*else udiff*/;
    this.fileHash = this.e.table.dataset.lefthash;
    tr.$chunker = this /* keep GC from reaping this */;
    this.pos = {
      /* These line numbers correspond to the LHS file. Because the
         contents are common to both sides, we have the same number
         for the RHS, but need to extract those line numbers from the
         neighboring TR blocks */
      startLhs: +tr.dataset.startln,
      endLhs: +tr.dataset.endln
    };
    D.clearElement(tr);
    this.e.td = D.addClass(
      /* Holder for our UI controls */
      D.attr(D.td(tr), 'colspan', this.isSplit ? 5 : 4),
      'chunkctrl'
    );
    this.e.btnWrapper = D.div();
    D.append(this.e.td, this.e.btnWrapper);
    /**
       Depending on various factors, we need one or more of:

       - A single button to load the initial chunk incrementally

       - A single button to load all lines then remove this control

       - Two buttons: one to load upwards, one to load downwards

       - A single button to load the final chunk incrementally
    */
    if(tr.nextElementSibling){
      this.pos.next = {
        startLhs: extractLineNo(true, true, tr.nextElementSibling, this.isSplit),
        startRhs: extractLineNo(false, true, tr.nextElementSibling, this.isSplit)
      };
    }
    if(tr.previousElementSibling){
      this.pos.prev = {
        endLhs: extractLineNo(true, false, tr.previousElementSibling, this.isSplit),
        endRhs: extractLineNo(false, false, tr.previousElementSibling, this.isSplit)
      };
    }
    let btnUp = false, btnDown = false;
    /**
       this.pos.next refers to the line numbers in the next TR's chunk.
       this.pos.prev refers to the line numbers in the previous TR's chunk.
    */
    if(this.pos.prev && this.pos.next
       && ((this.pos.next.startLhs - this.pos.prev.endLhs)
           <= Diff.config.chunkLoadLines)){
      /* Place a single button to load the whole block, rather
         than separate up/down buttons. */
      btnDown = false;
      btnUp = this.createButton(this.FetchType.FillGap);
    }else{
      /* Figure out which chunk-load buttons to add... */
      if(this.pos.prev){
        btnDown = this.createButton(this.FetchType.PrevDown);
      }
      if(this.pos.next){
        btnUp = this.createButton(this.FetchType.NextUp);
      }
    }
    //this.e.btnUp = btnUp;
    //this.e.btnDown = btnDown;
    if(btnDown) D.append(this.e.btnWrapper, btnDown);
    if(btnUp) D.append(this.e.btnWrapper, btnUp);
    /* For debugging only... */
    this.e.posState = D.span();
    D.append(this.e.btnWrapper, this.e.posState);
    this.updatePosDebug();
  };

  ChunkLoadControls.prototype = {
    /** An "enum" of values describing the types of context
        fetches/operations performed by this type. The values in this
        object must not be changed without modifying all logic which
        relies on their relative order. */
    FetchType:{
      /** Append context to the bottom of the previous diff chunk. */
      PrevDown: 1,
      /** Fill a complete gap between the previous/next diff chunks
          or at the start of the next chunk or end of the previous
          chunks. */
      FillGap: 0,
      /** Prepend context to the start of the next diff chunk. */
      NextUp: -1
    },
    config: {
      /*
      glyphUp: '⇡', //'&#uarr;',
      glyphDown: '⇣' //'&#darr;'
      */
    },

    /**
       Creates and returns a button element for fetching a chunk in
       the given fetchType (as documented for fetchChunk()).
    */
    createButton: function(fetchType){
      let b;
      switch(fetchType){
      case this.FetchType.PrevDown:
        b = D.append(
          D.addClass(D.span(), 'down'),
          D.span(/*glyph holder*/)
        );
        break;
      case this.FetchType.FillGap:
        b = D.append(
          D.addClass(D.span(), 'up', 'down'),
          D.span(/*glyph holder*/)
        );
        break;
      case this.FetchType.NextUp:
        b = D.append(
          D.addClass(D.span(), 'up'),
          D.span(/*glyph holder*/)
        );
        break;
      default:
        throw new Error("Internal API misuse: unexpected fetchType value "+fetchType);
      }
      D.addClass(b, 'jcbutton');
      b.addEventListener('click', ()=>this.fetchChunk(fetchType),false);
      return b;
    },

    updatePosDebug: function(){
      if(this.e.posState){
        D.clearElement(this.e.posState);
        //D.append(D.clearElement(this.e.posState), JSON.stringify(this.pos));
      }
      return this;
    },

    /* Attempt to clean up resources and remove some circular references to
       that GC can do the right thing. */
    destroy: function(){
      D.remove(this.e.tr);
      delete this.e.tr.$chunker;
      delete this.e.tr;
      delete this.e;
      delete this.pos;
    },

    /**
       If the gap between this.pos.endLhs/startLhs is less than or equal to
       Diff.config.chunkLoadLines then this function replaces any up/down buttons
       with a gap-filler button, else it's a no-op. Returns this object.

       As a special case, do not apply this at the start or bottom
       of the diff, only between two diff chunks.
    */
    maybeReplaceButtons: function(){
      if(this.pos.next && this.pos.prev
         && (this.pos.endLhs - this.pos.startLhs <= Diff.config.chunkLoadLines)){
        D.clearElement(this.e.btnWrapper);
        D.append(this.e.btnWrapper, this.createButton(this.FetchType.FillGap));
      }
      return this;
    },

    /**
       Callack for /jchunk responses.
    */
    injectResponse: function f(fetchType/*as for fetchChunk()*/,
                               urlParam/*from fetchChunk()*/,
                               lines/*response lines*/){
      if(!lines.length){
        /* No more data to load */
        this.destroy();
        return this;
      }
      //console.debug("Loaded line range ",
      //urlParam.from,"-",urlParam.to, "fetchType ",fetchType);
      const lineno = [],
            trPrev = this.e.tr.previousElementSibling,
            trNext = this.e.tr.nextElementSibling,
            doAppend = (
              !!trPrev && fetchType>=this.FetchType.FillGap
            ) /* true to append to previous TR, else prepend to NEXT TR */;
      const tr = doAppend ? trPrev : trNext;
      const joinTr = (
        this.FetchType.FillGap===fetchType && trPrev && trNext
      ) ? trNext : false
      /* Truthy if we want to combine trPrev, the new content, and
         trNext into trPrev and then remove trNext. */;
      let i, td;
      if(!f.convertLines){
        f.convertLines = function(li){
          return li.join('\n')
            .replaceAll('&','&amp;')
            .replaceAll('<','&lt;')+'\n';
        };
      }
      if(1){ // LHS line numbers...
        const selector = '.difflnl > pre';
        td = tr.querySelector(selector);
        const lnTo = Math.min(urlParam.to,
                              urlParam.from +
                              lines.length - 1/*b/c request range is inclusive*/);
        for( i = urlParam.from; i <= lnTo; ++i ){
          lineno.push(i);
        }
        const lineNoTxt = lineno.join('\n')+'\n';
        const content = [td.innerHTML];
        if(doAppend) content.push(lineNoTxt);
        else content.unshift(lineNoTxt);
        if(joinTr){
          content.push(trNext.querySelector(selector).innerHTML);
        }
        td.innerHTML = content.join('');
      }

      if(1){// code block(s)...
        const selector = '.difftxt > pre';
        td = tr.querySelectorAll(selector);
        const code = f.convertLines(lines);
        let joinNdx = 0;
        td.forEach(function(e){
          const content = [e.innerHTML];
          if(doAppend) content.push(code);
          else content.unshift(code);
          if(joinTr){
            content.push(trNext.querySelectorAll(selector)[joinNdx++].innerHTML)
          }
          e.innerHTML = content.join('');
        });
      }

      if(1){// Add blank lines in (.diffsep>pre)
        const selector = '.diffsep > pre';
        td = tr.querySelector(selector);
        for(i = 0; i < lineno.length; ++i) lineno[i] = '';
        const blanks = lineno.join('\n')+'\n';
        const content = [td.innerHTML];
        if(doAppend) content.push(blanks);
        else content.unshift(blanks);
        if(joinTr){
          content.push(trNext.querySelector(selector).innerHTML);
        }
        td.innerHTML = content.join('');
      }

      if(this.FetchType.FillGap===fetchType){
        /* Closing the whole gap between two chunks or a whole gap
           at the start or end of a diff. */
        // RHS line numbers...
        let startLnR = this.pos.prev
            ? this.pos.prev.endRhs+1 /* Closing the whole gap between two chunks
                                        or end-of-file gap. */
            : this.pos.next.startRhs - lines.length /* start-of-file gap */;
        lineno.length = lines.length;
        for( i = startLnR; i < startLnR + lines.length; ++i ){
          lineno[i-startLnR] = i;
        }
        const selector = '.difflnr > pre';
        td = tr.querySelector(selector);
        const lineNoTxt = lineno.join('\n')+'\n';
        lineno.length = 0;
        const content = [td.innerHTML];
        if(doAppend) content.push(lineNoTxt);
        else content.unshift(lineNoTxt);
        if(joinTr){
          content.push(trNext.querySelector(selector).innerHTML);
        }
        td.innerHTML = content.join('');
        if(joinTr) D.remove(joinTr);
        Diff.checkTableWidth(true);
        this.destroy();
        return this;
      }else if(this.FetchType.PrevDown===fetchType){
        /* Append context to previous TR. */
        // RHS line numbers...
        let startLnR = this.pos.prev.endRhs+1;
        lineno.length = lines.length;
        for( i = startLnR; i < startLnR + lines.length; ++i ){
          lineno[i-startLnR] = i;
        }
        this.pos.startLhs += lines.length;
        this.pos.prev.endRhs += lines.length;
        this.pos.prev.endLhs += lines.length;
        const selector = '.difflnr > pre';
        td = tr.querySelector(selector);
        const lineNoTxt = lineno.join('\n')+'\n';
        lineno.length = 0;
        const content = [td.innerHTML];
        if(doAppend) content.push(lineNoTxt);
        else content.unshift(lineNoTxt);
        td.innerHTML = content.join('');
        if(lines.length < (urlParam.to - urlParam.from)){
          /* No more data. */
          this.destroy();
        }else{
          this.maybeReplaceButtons();
          this.updatePosDebug();
        }
        Diff.checkTableWidth(true);
        return this;
      }else if(this.FetchType.NextUp===fetchType){
        /* Prepend content to next TR. */
        // RHS line numbers...
        if(doAppend){
          throw new Error("Internal precondition violation: doAppend is true.");
        }
        let startLnR = this.pos.next.startRhs - lines.length;
        lineno.length = lines.length;
        for( i = startLnR; i < startLnR + lines.length; ++i ){
          lineno[i-startLnR] = i;
        }
        this.pos.endLhs -= lines.length;
        this.pos.next.startRhs -= lines.length;
        this.pos.next.startLhs -= lines.length;
        const selector = '.difflnr > pre';
        td = tr.querySelector(selector);
        const lineNoTxt = lineno.join('\n')+'\n';
        lineno.length = 0;
        td.innerHTML = lineNoTxt + td.innerHTML;
        if(this.pos.endLhs<=1
           || lines.length < (urlParam.to - urlParam.from)){
          /* No more data. */
          this.destroy();
        }else{
          this.maybeReplaceButtons();
          this.updatePosDebug();
        }
        Diff.checkTableWidth(true);
        return this;
      }else{
        throw new Error("Unexpected 'fetchType' value.");
      }
    },

    /**
       Fetches and inserts a line chunk. fetchType is:

       this.FetchType.NextUp = upwards from next chunk (this.pos.next)

       this.FetchType.FillGap = the whole gap between this.pos.prev
       and this.pos.next, or the whole gap before/after the
       initial/final chunk in the diff.

       this.FetchType.PrevDown = downwards from the previous chunk
       (this.pos.prev)

       Those values are set at the time this object is initialized but
       one instance of this class may have 2 buttons, one each for
       fetchTypes NextUp and PrevDown.

       This is an async operation. While it is in transit, any calls
       to this function will have no effect except (possibly) to emit
       a warning. Returns this object.
    */
    fetchChunk: function(fetchType){
      /* Forewarning, this is a bit confusing: when fetching the
         previous lines, we're doing so on behalf of the *next* diff
         chunk (this.pos.next), and vice versa. */
      if(this.$isFetching){
        F.toast.warning("Cannot load chunk while a load is pending.");
        return this;
      }
      if(fetchType===this.FetchType.NextUp && !this.pos.next
        || fetchType===this.FetchType.PrevDown && !this.pos.prev){
        console.error("Attempt to fetch diff lines but don't have any.");
        return this;
      }
      const fOpt = {
        urlParams:{
          name: this.fileHash, from: 0, to: 0
        },
        aftersend: ()=>delete this.$isFetching,
        onload: (list)=>this.injectResponse(fetchType,up,list)
      };
      const up = fOpt.urlParams;
      if(fetchType===this.FetchType.FillGap){
        /* Easiest case: filling a whole gap. */
        up.from = this.pos.startLhs;
        up.to = this.pos.endLhs;
      }else if(this.FetchType.PrevDown===fetchType){
        /* Append to previous TR. */
        if(!this.pos.prev){
          console.error("Attempt to fetch next diff lines but don't have any.");
          return this;
        }
        up.from = this.pos.prev.endLhs + 1;
        up.to = up.from +
          Diff.config.chunkLoadLines - 1/*b/c request range is inclusive*/;
        if( this.pos.next && this.pos.next.startLhs <= up.to ){
          up.to = this.pos.next.startLhs - 1;
          fetchType = this.FetchType.FillGap;
        }
      }else{
        /* Prepend to next TR */
        if(!this.pos.next){
          console.error("Attempt to fetch previous diff lines but don't have any.");
          return this;
        }
        up.to = this.pos.next.startLhs - 1;
        up.from = Math.max(1, up.to - Diff.config.chunkLoadLines + 1);
        if( this.pos.prev && this.pos.prev.endLhs >= up.from ){
          up.from = this.pos.prev.endLhs + 1;
          fetchType = this.FetchType.FillGap;
        }
      }
      this.$isFetching = true;
      //console.debug("fetchChunk(",fetchType,")",up);
      Diff.fetchArtifactChunk(fOpt);
      return this;
    }
  };

  /**
     Adds context-loading buttons to one or more tables. The argument
     may be a forEach-capable list of diff table elements, a query
     selector string matching 0 or more diff tables, or falsy, in
     which case all relevant diff tables are set up. It tags each
     table it processes to that it will not be processed multiple
     times by subsequent calls to this function.

     Note that this only works for diffs which have been marked up
     with certain state, namely table.dataset.lefthash and TR
     entries which hold state related to browsing context.
  */
  Diff.setupDiffContextLoad = function(tables){
    if('string'===typeof tables){
      tables = document.querySelectorAll(tables);
    }else if(!tables){
      tables = document.querySelectorAll('table.diff[data-lefthash]:not(.diffskipped)');
    }
    /* Potential performance-related TODO: instead of installing all
       of these at once, install them as the corresponding TR is
       scrolled into view. */
    tables.forEach(function(table){
      if(table.classList.contains('diffskipped') || !table.dataset.lefthash) return;
      D.addClass(table, 'diffskipped'/*avoid processing these more than once */);
      table.querySelectorAll('tr.diffskip[data-startln]').forEach(function(tr){
        new ChunkLoadControls(D.addClass(tr, 'jchunk'));
      });
    });
    return F;
  };
  Diff.setupDiffContextLoad();
});

/* Refinements to the display of unified and side-by-side diffs.
**
** In all cases, the table columns tagged with "difftxt" are expanded,
** where possible, to fill the width of the screen.
**
** For a side-by-side diff, if either column is two wide to fit on the
** display, scrollbars are added.  The scrollbars are linked, so that
** both sides scroll together.  Left and right arrows also scroll.
*/
window.fossil.onPageLoad(function(){
  const SCROLL_LEN = 25;
  const F = window.fossil, D = F.dom, Diff = F.diff;
  var lastWidth;
  Diff.checkTableWidth = function f(force){
    if(undefined === f.contentNode){
      f.contentNode = document.querySelector('div.content');
    }
    force = true;
    const parentCS = window.getComputedStyle(f.contentNode);
    const parentWidth = (
      //document.body.clientWidth;
      //parentCS.width;
      f.contentNode.clientWidth
        - parseFloat(parentCS.marginLeft) - parseFloat(parentCS.marginRight)
    );
    if( !force && parentWidth===lastWidth ) return this;
    lastWidth = parentWidth;
    let w = lastWidth*0.5 - 100;
    //console.debug( "w = ",w,", lastWidth =",lastWidth," body = ",document.body.clientWidth);
    if(force || !f.colsL){
      f.colsL = document.querySelectorAll('td.difftxtl pre');
    }
    f.colsL.forEach(function(e){
      e.style.width = w + "px";
      e.style.maxWidth = w + "px";
    });
    if(force || !f.colsR){
      f.colsR = document.querySelectorAll('td.difftxtr pre');
    }
    f.colsR.forEach(function(e){
      e.style.width = w + "px";
      e.style.maxWidth = w + "px";
    });
    if(0){ // seems to be unnecessary
      if(!f.allDiffs){
        f.allDiffs = document.querySelectorAll('table.diff');
      }
      w = lastWidth;
      f.allDiffs.forEach(function f(e){
        if(0 && !f.$){
          f.$ = e.getClientRects()[0];
          console.debug("diff table w =",w," f.$x",f.$);
          w - 2*f.$.x /* left margin (assume right==left, for simplicity) */;
        }
        e.style.maxWidth = w + "px";
      });
      //console.debug("checkTableWidth(",force,") lastWidth =",lastWidth);
    }
    return this;
  };

  const scrollLeft = function(event){
    //console.debug("scrollLeft",this,event);
    const table = this.parentElement/*TD*/.parentElement/*TR*/.
      parentElement/*TBODY*/.parentElement/*TABLE*/;
    table.$txtPres.forEach((e)=>(e===this) ? 1 : (e.scrollLeft = this.scrollLeft));
    return false;
  };
  Diff.initTableDiff = function f(diff){
    if(!diff){
      let i, diffs = document.querySelectorAll('table.splitdiff');
      for(i=0; i<diffs.length; ++i){
        f.call(this, diffs[i]);
      }
      return this;
    }
    diff.$txtCols = diff.querySelectorAll('td.difftxt');
    diff.$txtPres = diff.querySelectorAll('td.difftxt pre');
    var width = 0;
    diff.$txtPres.forEach(function(e){
      if(width < e.scrollWidth) width = e.scrollWidth;
    });
    //console.debug("diff.$txtPres =",diff.$txtPres);
    diff.$txtCols.forEach((e)=>e.style.width = width + 'px');
    diff.$txtPres.forEach(function(e){
      e.style.maxWidth = width + 'px';
      e.style.width = width + 'px';
      if(!e.classList.contains('scroller')){
        D.addClass(e, 'scroller');
        e.addEventListener('scroll', scrollLeft, false);
      }
    });
    diff.tabIndex = 0;
    if(!diff.classList.contains('scroller')){
      D.addClass(diff, 'scroller');
      diff.addEventListener('keydown', function(e){
        e = e || event;
        const len = {37: -SCROLL_LEN, 39: SCROLL_LEN}[e.keyCode];
        if( !len ) return;
        this.$txtPres[0].scrollLeft += len;
        /* ^^^ bug: if there is a 2nd column and it has a scrollbar
           but txtPres[0] does not, no scrolling happens here. We need
           to find the widest of txtPres and scroll that one. Example:
           Checkin a7fbefee38a1c522 file diff.c */
        return false;
      }, false);
    }
    return this;
  }
  window.fossil.page.tweakSbsDiffs = function(){
    document.querySelectorAll('table.splitdiff').forEach((e)=>Diff.initTableDiff);
  };
  Diff.initTableDiff().checkTableWidth();
  window.addEventListener('resize', ()=>Diff.checkTableWidth());
}, false);