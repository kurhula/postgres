/*-------------------------------------------------------------------------
 *
 * gistxlog.c
 *	  WAL replay logic for GiST.
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *           $PostgreSQL: pgsql/src/backend/access/gist/gistxlog.c,v 1.2 2005/06/20 10:29:36 teodor Exp $
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/gist_private.h"
#include "access/gistscan.h"
#include "access/heapam.h"
#include "catalog/index.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "utils/memutils.h"


typedef struct {
	gistxlogEntryUpdate	*data;
	int			len;
	IndexTuple		*itup;
	BlockNumber		*path;
	OffsetNumber		*todelete;
} EntryUpdateRecord;

typedef struct {
	gistxlogPage	*header;
	OffsetNumber	*offnum;

	/* to work with */
	Page	page;
	Buffer	buffer;
	bool	is_ok;
} NewPage;

typedef struct {
	gistxlogPageSplit	*data;
	NewPage			*page;
	IndexTuple		*itup;
	BlockNumber		*path;
	OffsetNumber		*todelete;
} PageSplitRecord;

/* track for incomplete inserts, idea was taken from nbtxlog.c */

typedef struct gistIncompleteInsert {
	RelFileNode	node;
	ItemPointerData	key;
	int		lenblk;
	BlockNumber	*blkno;
	int		pathlen;
	BlockNumber	*path;
	XLogRecPtr	lsn;
} gistIncompleteInsert;


MemoryContext opCtx;
MemoryContext insertCtx;
static List *incomplete_inserts;


#define ItemPointerEQ( a, b )	\
	( \
	ItemPointerGetOffsetNumber(a) == ItemPointerGetOffsetNumber(b) && \
	ItemPointerGetBlockNumber (a) == ItemPointerGetBlockNumber(b) \
        )

static void
pushIncompleteInsert(RelFileNode node, XLogRecPtr lsn, ItemPointerData key,
		BlockNumber *blkno, int lenblk,
		BlockNumber *path,  int pathlen,
		PageSplitRecord *xlinfo /* to extract blkno info */ ) {
	MemoryContext oldCxt = MemoryContextSwitchTo(insertCtx);
	gistIncompleteInsert *ninsert = (gistIncompleteInsert*)palloc( sizeof(gistIncompleteInsert) );

	ninsert->node = node;
	ninsert->key  = key;
	ninsert->lsn  = lsn;

	if ( lenblk && blkno ) {	
		ninsert->lenblk = lenblk;
		ninsert->blkno = (BlockNumber*)palloc( sizeof(BlockNumber)*ninsert->lenblk );
		memcpy(ninsert->blkno, blkno, sizeof(BlockNumber)*ninsert->lenblk);
	} else {
		int i;

		Assert( xlinfo );
		ninsert->lenblk = xlinfo->data->npage;
		ninsert->blkno = (BlockNumber*)palloc( sizeof(BlockNumber)*ninsert->lenblk );
		for(i=0;i<ninsert->lenblk;i++)
			ninsert->blkno[i] = xlinfo->page[i].header->blkno;
	}
	Assert( ninsert->lenblk>0 );
	
	if ( path && pathlen ) {
		ninsert->pathlen = pathlen;
		ninsert->path = (BlockNumber*)palloc( sizeof(BlockNumber)*ninsert->pathlen );
		memcpy(ninsert->path, path, sizeof(BlockNumber)*ninsert->pathlen);
	} else { 
		ninsert->pathlen = 0;
		ninsert->path = NULL;
	}

	incomplete_inserts = lappend(incomplete_inserts, ninsert);
	MemoryContextSwitchTo(oldCxt);
}

static void
forgetIncompleteInsert(RelFileNode node, ItemPointerData key) {
	ListCell   *l;

	foreach(l, incomplete_inserts) {
		gistIncompleteInsert	*insert = (gistIncompleteInsert*) lfirst(l);

		if (  RelFileNodeEquals(node, insert->node) && ItemPointerEQ( &(insert->key), &(key) ) ) {
			
			/* found */
			if ( insert->path ) pfree( insert->path );
			pfree( insert->blkno );
			incomplete_inserts = list_delete_ptr(incomplete_inserts, insert);
			pfree( insert );
			break;
		} 
	}
}

static void
decodeEntryUpdateRecord(EntryUpdateRecord *decoded, XLogRecord *record) {
	char *begin = XLogRecGetData(record), *ptr;
	int i=0, addpath=0;

	decoded->data = (gistxlogEntryUpdate*)begin;

	if ( decoded->data->pathlen ) {
		addpath = MAXALIGN( sizeof(BlockNumber) * decoded->data->pathlen );
		decoded->path = (BlockNumber*)(begin+sizeof( gistxlogEntryUpdate ));
	} else 
		decoded->path = NULL;

	if ( decoded->data->ntodelete ) {
		decoded->todelete = (OffsetNumber*)(begin + sizeof( gistxlogEntryUpdate ) + addpath);
		addpath += MAXALIGN( sizeof(OffsetNumber) * decoded->data->ntodelete );
	} else 
		decoded->todelete = NULL;	

	decoded->len=0;
	ptr=begin+sizeof( gistxlogEntryUpdate ) + addpath;
	while( ptr - begin < record->xl_len ) {
		decoded->len++;
		ptr += IndexTupleSize( (IndexTuple)ptr );
	}  

	decoded->itup=(IndexTuple*)palloc( sizeof( IndexTuple ) * decoded->len );
	
	ptr=begin+sizeof( gistxlogEntryUpdate ) + addpath;
	while( ptr - begin < record->xl_len ) {
		decoded->itup[i] = (IndexTuple)ptr;
		ptr += IndexTupleSize( decoded->itup[i] );
		i++;
	}
}

/*
 * redo any page update (except page split)
 */
static void
gistRedoEntryUpdateRecord(XLogRecPtr lsn, XLogRecord *record, bool isnewroot) {
	EntryUpdateRecord	xlrec;
	Relation        reln;
	Buffer          buffer;
	Page            page;

	decodeEntryUpdateRecord( &xlrec, record );

	reln = XLogOpenRelation(xlrec.data->node);
	if (!RelationIsValid(reln))
		return;
	buffer = XLogReadBuffer(false, reln, xlrec.data->blkno);
	if (!BufferIsValid(buffer))
		elog(PANIC, "gistRedoEntryUpdateRecord: block unfound");
	page = (Page) BufferGetPage(buffer);

	if ( isnewroot ) {
		if ( !PageIsNew((PageHeader) page) && XLByteLE(lsn, PageGetLSN(page)) ) {
			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
			ReleaseBuffer(buffer);
			return;
		}
	} else { 
		if ( PageIsNew((PageHeader) page) )
			elog(PANIC, "gistRedoEntryUpdateRecord: uninitialized page");
		if (XLByteLE(lsn, PageGetLSN(page))) {
			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
			ReleaseBuffer(buffer);
			return;
		}
	}

	if ( xlrec.data->isemptypage ) {
		while( !PageIsEmpty(page) )
			PageIndexTupleDelete( page, FirstOffsetNumber );
		
		if ( xlrec.data->blkno == GIST_ROOT_BLKNO )
			GistPageSetLeaf( page );
		else
			GistPageSetDeleted( page );
	} else {
		if ( isnewroot )
			GISTInitBuffer(buffer, 0);
		else if ( xlrec.data->ntodelete ) { 
			int i;
			for(i=0; i < xlrec.data->ntodelete ; i++)  
				PageIndexTupleDelete(page, xlrec.todelete[i]);
			if ( GistPageIsLeaf(page) )
				GistMarkTuplesDeleted(page);
		}

		/* add tuples */
		if ( xlrec.len > 0 ) {
        	        OffsetNumber off = (PageIsEmpty(page)) ?  
        	                FirstOffsetNumber
        	                :
        	                OffsetNumberNext(PageGetMaxOffsetNumber(page));

			gistfillbuffer(reln, page, xlrec.itup, xlrec.len, off);
		}

		/* special case: leafpage, nothing to insert, nothing to delete, then
		   vacuum marks page */
		if ( GistPageIsLeaf(page) && xlrec.len == 0 && xlrec.data->ntodelete == 0 )
			GistClearTuplesDeleted(page);	
	}

	PageSetLSN(page, lsn);
	PageSetTLI(page, ThisTimeLineID);
	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	WriteBuffer(buffer);

	if ( ItemPointerIsValid( &(xlrec.data->key) ) ) {
		if ( incomplete_inserts != NIL )
			forgetIncompleteInsert(xlrec.data->node, xlrec.data->key);

		if ( !isnewroot && xlrec.data->blkno!=GIST_ROOT_BLKNO )
			pushIncompleteInsert(xlrec.data->node, lsn, xlrec.data->key, 
				&(xlrec.data->blkno), 1,
				xlrec.path, xlrec.data->pathlen,
				NULL);
	}
}

static void
decodePageSplitRecord(PageSplitRecord *decoded, XLogRecord *record) {
	char *begin = XLogRecGetData(record), *ptr;
	int i=0, addpath = 0;

	decoded->data = (gistxlogPageSplit*)begin;
	decoded->page = (NewPage*)palloc( sizeof(NewPage) * decoded->data->npage );
	decoded->itup = (IndexTuple*)palloc( sizeof(IndexTuple) * decoded->data->nitup );

	if ( decoded->data->pathlen ) {
		addpath = MAXALIGN( sizeof(BlockNumber) * decoded->data->pathlen );
		decoded->path = (BlockNumber*)(begin+sizeof( gistxlogPageSplit ));
	} else 
		decoded->path = NULL;

	if ( decoded->data->ntodelete ) {
		decoded->todelete = (OffsetNumber*)(begin + sizeof( gistxlogPageSplit ) + addpath);
		addpath += MAXALIGN( sizeof(OffsetNumber) * decoded->data->ntodelete );
	} else 
		decoded->todelete = NULL;	

	ptr=begin+sizeof( gistxlogPageSplit ) + addpath;
	for(i=0;i<decoded->data->nitup;i++) {
		Assert( ptr - begin < record->xl_len );
		decoded->itup[i] = (IndexTuple)ptr;
		ptr += IndexTupleSize( decoded->itup[i] );
	}

	for(i=0;i<decoded->data->npage;i++) {
		Assert( ptr - begin < record->xl_len );
		decoded->page[i].header = (gistxlogPage*)ptr;
		ptr += sizeof(gistxlogPage);

		Assert( ptr - begin < record->xl_len );
		decoded->page[i].offnum = (OffsetNumber*)ptr;
		ptr += MAXALIGN( sizeof(OffsetNumber) * decoded->page[i].header->num );
	}
}
	
static void
gistRedoPageSplitRecord(XLogRecPtr lsn, XLogRecord *record ) {
	PageSplitRecord	xlrec;
	Relation        reln;
	Buffer          buffer;
	Page            page;
	int 		i, len=0;
	IndexTuple	*itup, *institup;
	GISTPageOpaque opaque;
	bool release=true;

	decodePageSplitRecord( &xlrec, record );

	reln = XLogOpenRelation(xlrec.data->node);
	if (!RelationIsValid(reln))
		return;
	buffer = XLogReadBuffer( false, reln, xlrec.data->origblkno);
	if (!BufferIsValid(buffer))
		elog(PANIC, "gistRedoEntryUpdateRecord: block unfound");
	page = (Page) BufferGetPage(buffer);
	if (PageIsNew((PageHeader) page))
		elog(PANIC, "gistRedoEntryUpdateRecord: uninitialized page");

	if (XLByteLE(lsn, PageGetLSN(page))) {
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buffer);
		return;
	}
	
	if ( xlrec.data->ntodelete ) { 
		int i;
		for(i=0; i < xlrec.data->ntodelete ; i++)  
			PageIndexTupleDelete(page, xlrec.todelete[i]);
	}

	itup = gistextractbuffer(buffer, &len);
	itup = gistjoinvector(itup, &len, xlrec.itup, xlrec.data->nitup);
	institup = (IndexTuple*)palloc( sizeof(IndexTuple) * len );
        opaque = (GISTPageOpaque) PageGetSpecialPointer(page);

	/* read and fill all pages */
	for(i=0;i<xlrec.data->npage;i++) {
		int j;
		NewPage *newpage = xlrec.page + i; 

		/* prepare itup vector per page */
		for(j=0;j<newpage->header->num;j++)
			institup[j] = itup[ newpage->offnum[j] - 1 ];

		if ( newpage->header->blkno == xlrec.data->origblkno ) {
			/* IncrBufferRefCount(buffer); */
			newpage->page = (Page) PageGetTempPage(page, sizeof(GISTPageOpaqueData));
			newpage->buffer = buffer;
			newpage->is_ok=false; 
		} else {
			newpage->buffer = XLogReadBuffer(true, reln, newpage->header->blkno);
			if (!BufferIsValid(newpage->buffer))
				elog(PANIC, "gistRedoPageSplitRecord: lost page");
			newpage->page = (Page) BufferGetPage(newpage->buffer);
			if (!PageIsNew((PageHeader) newpage->page) && XLByteLE(lsn, PageGetLSN(newpage->page))) {
				LockBuffer(newpage->buffer, BUFFER_LOCK_UNLOCK);
				ReleaseBuffer(newpage->buffer);
				newpage->is_ok=true;
				continue; /* good page */
			} else {
				newpage->is_ok=false;
				GISTInitBuffer(newpage->buffer, opaque->flags & F_LEAF);
			}
		}
		gistfillbuffer(reln, newpage->page, institup, newpage->header->num, FirstOffsetNumber);
	}

	for(i=0;i<xlrec.data->npage;i++) {
		NewPage *newpage = xlrec.page + i;

		if ( newpage->is_ok )
			continue;

		if ( newpage->header->blkno == xlrec.data->origblkno ) { 
			PageRestoreTempPage(newpage->page, page);
			release = false;
		}

		PageSetLSN(newpage->page, lsn);
		PageSetTLI(newpage->page, ThisTimeLineID);
		LockBuffer(newpage->buffer, BUFFER_LOCK_UNLOCK);
		WriteBuffer(newpage->buffer);	
	}

	if ( release ) {
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buffer);
	}

	if ( ItemPointerIsValid( &(xlrec.data->key) ) ) {
		if ( incomplete_inserts != NIL )
			forgetIncompleteInsert(xlrec.data->node, xlrec.data->key);

		pushIncompleteInsert(xlrec.data->node, lsn, xlrec.data->key, 
				NULL, 0,
				xlrec.path, xlrec.data->pathlen,
				&xlrec);
	}
}

static void
gistRedoCreateIndex(XLogRecPtr lsn, XLogRecord *record) {
	RelFileNode	*node = (RelFileNode*)XLogRecGetData(record);
	Relation        reln;
	Buffer          buffer;
	Page            page;

	reln = XLogOpenRelation(*node);
	if (!RelationIsValid(reln))
		return;
	buffer = XLogReadBuffer( true, reln, GIST_ROOT_BLKNO);
	if (!BufferIsValid(buffer))
		elog(PANIC, "gistRedoCreateIndex: block unfound");
	page = (Page) BufferGetPage(buffer);

	if (!PageIsNew((PageHeader) page) && XLByteLE(lsn, PageGetLSN(page))) {
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buffer);
		return;
	}

	GISTInitBuffer(buffer, F_LEAF);

	PageSetLSN(page, lsn);
	PageSetTLI(page, ThisTimeLineID);
	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	WriteBuffer(buffer);	
}

static void
gistRedoCompleteInsert(XLogRecPtr lsn, XLogRecord *record) {
	char *begin = XLogRecGetData(record), *ptr;
	gistxlogInsertComplete	*xlrec;

	xlrec = (gistxlogInsertComplete*)begin;

	ptr = begin + sizeof( gistxlogInsertComplete );
	while( ptr - begin < record->xl_len ) {
		Assert( record->xl_len - (ptr - begin) >= sizeof(ItemPointerData) );
		forgetIncompleteInsert( xlrec->node, *((ItemPointerData*)ptr) );
		ptr += sizeof(ItemPointerData);
	}  
}

void
gist_redo(XLogRecPtr lsn, XLogRecord *record)
{
	uint8           info = record->xl_info & ~XLR_INFO_MASK;

	MemoryContext oldCxt;
	oldCxt = MemoryContextSwitchTo(opCtx);
	switch (info) {
		case	XLOG_GIST_ENTRY_UPDATE:
		case	XLOG_GIST_ENTRY_DELETE:
			gistRedoEntryUpdateRecord(lsn, record,false);
			break;
		case	XLOG_GIST_NEW_ROOT:
			gistRedoEntryUpdateRecord(lsn, record,true);
			break;
		case	XLOG_GIST_PAGE_SPLIT:
			gistRedoPageSplitRecord(lsn, record);	
			break;
		case	XLOG_GIST_CREATE_INDEX:
			gistRedoCreateIndex(lsn, record);
			break;
		case	XLOG_GIST_INSERT_COMPLETE:
			gistRedoCompleteInsert(lsn, record);
			break;
		default:
			elog(PANIC, "gist_redo: unknown op code %u", info);
	}

	MemoryContextSwitchTo(oldCxt);
	MemoryContextReset(opCtx);
}

static void
out_target(char *buf, RelFileNode node, ItemPointerData key)
{
        sprintf(buf + strlen(buf), "rel %u/%u/%u; tid %u/%u",
                 node.spcNode, node.dbNode, node.relNode,
                        ItemPointerGetBlockNumber(&key),
                        ItemPointerGetOffsetNumber(&key));
}

static void
out_gistxlogEntryUpdate(char *buf, gistxlogEntryUpdate *xlrec) {
	out_target(buf, xlrec->node, xlrec->key);
	sprintf(buf + strlen(buf), "; block number %u", 
		xlrec->blkno);
}

static void
out_gistxlogPageSplit(char *buf, gistxlogPageSplit *xlrec) {
	strcat(buf, "page_split: ");
	out_target(buf, xlrec->node, xlrec->key);
	sprintf(buf + strlen(buf), "; block number %u; add %d tuples; split to %d pages", 
		xlrec->origblkno, 
		xlrec->nitup, xlrec->npage);
}

void
gist_desc(char *buf, uint8 xl_info, char *rec)
{
	uint8           info = xl_info & ~XLR_INFO_MASK;

	switch (info) {
		case	XLOG_GIST_ENTRY_UPDATE:
			strcat(buf, "entry_update: ");
			out_gistxlogEntryUpdate(buf, (gistxlogEntryUpdate*)rec);
			break;
		case	XLOG_GIST_ENTRY_DELETE:
			strcat(buf, "entry_delete: ");
			out_gistxlogEntryUpdate(buf, (gistxlogEntryUpdate*)rec);
			break;
		case	XLOG_GIST_NEW_ROOT:
			strcat(buf, "new_root: ");
			out_target(buf, ((gistxlogEntryUpdate*)rec)->node, ((gistxlogEntryUpdate*)rec)->key);
			break;
		case	XLOG_GIST_PAGE_SPLIT:
			out_gistxlogPageSplit(buf, (gistxlogPageSplit*)rec);
			break;
		case	XLOG_GIST_CREATE_INDEX:
			sprintf(buf + strlen(buf), "create_index: rel %u/%u/%u", 
				((RelFileNode*)rec)->spcNode, 
				((RelFileNode*)rec)->dbNode, 
				((RelFileNode*)rec)->relNode);
			break;
		case	XLOG_GIST_INSERT_COMPLETE:
			sprintf(buf + strlen(buf), "complete_insert: rel %u/%u/%u", 
				((gistxlogInsertComplete*)rec)->node.spcNode, 
				((gistxlogInsertComplete*)rec)->node.dbNode, 
				((gistxlogInsertComplete*)rec)->node.relNode);
		default:
			elog(PANIC, "gist_desc: unknown op code %u", info);
	}
}

IndexTuple 
gist_form_invalid_tuple(BlockNumber blkno) {
	/* we don't alloc space for null's bitmap, this is invalid tuple,
	   be carefull in read and write code */
	Size size = IndexInfoFindDataOffset(0);
	IndexTuple tuple=(IndexTuple)palloc0( size );

	tuple->t_info |= size;
	
	ItemPointerSetBlockNumber(&(tuple->t_tid), blkno);
	GistTupleSetInvalid( tuple );

	return tuple;
}

static void
gistContinueInsert(gistIncompleteInsert *insert) {
	IndexTuple   *itup;
	int i, lenitup;
	MemoryContext oldCxt;
	Relation index;

	oldCxt = MemoryContextSwitchTo(opCtx);
	
	index = XLogOpenRelation(insert->node);
	if (!RelationIsValid(index))
		return;

	elog(LOG,"Detected incomplete insert into GiST index %u/%u/%u; It's desirable to vacuum or reindex index",
		 insert->node.spcNode, insert->node.dbNode, insert->node.relNode);

	/* needed vector itup never will be more than initial lenblkno+2, 
           because during this processing Indextuple can be only smaller */ 
	lenitup = insert->lenblk;	
	itup = (IndexTuple*)palloc(sizeof(IndexTuple)*(lenitup+2 /*guarantee root split*/));

	for(i=0;i<insert->lenblk;i++) 
		itup[i] = gist_form_invalid_tuple( insert->blkno[i] );

	if ( insert->pathlen==0 ) {
		/*it  was split root, so we should only make new root*/
        	Buffer buffer = XLogReadBuffer(true, index, GIST_ROOT_BLKNO);
        	Page   page;

		if (!BufferIsValid(buffer))
			elog(PANIC, "gistContinueInsert: root block unfound");

        	GISTInitBuffer(buffer, 0);
        	page = BufferGetPage(buffer);
        	gistfillbuffer(index, page, itup, lenitup, FirstOffsetNumber);
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
        	WriteBuffer(buffer);
	} else {
		Buffer	*buffers;
		Page	*pages;
		int numbuffer;
		
		buffers= (Buffer*) palloc( sizeof(Buffer) * (insert->lenblk+2/*guarantee root split*/) );
		pages  = (Page*)   palloc( sizeof(Page  ) * (insert->lenblk+2/*guarantee root split*/) );

		for(i=0;i<insert->pathlen;i++) {
			int	j, k, pituplen=0, childfound=0;
		
			numbuffer=1;
			buffers[numbuffer-1] = XLogReadBuffer(false, index, insert->path[i]);
			if (!BufferIsValid(buffers[numbuffer-1]))
				elog(PANIC, "gistContinueInsert: block %u unfound", insert->path[i]);
			pages[numbuffer-1] = BufferGetPage( buffers[numbuffer-1] );
			if ( PageIsNew((PageHeader)(pages[numbuffer-1])) )
				elog(PANIC, "gistContinueInsert: uninitialized page");

			pituplen = PageGetMaxOffsetNumber(pages[numbuffer-1]);
			
			/* remove old IndexTuples */
			for(j=0;j<pituplen && childfound<lenitup;j++) {
				BlockNumber blkno;
				ItemId iid = PageGetItemId(pages[numbuffer-1], j+FirstOffsetNumber);
                        	IndexTuple idxtup = (IndexTuple) PageGetItem(pages[numbuffer-1], iid);

				blkno = ItemPointerGetBlockNumber( &(idxtup->t_tid) );

				for(k=0;k<lenitup;k++) 
					if ( ItemPointerGetBlockNumber( &(itup[k]->t_tid) ) == blkno ) {
						PageIndexTupleDelete(pages[numbuffer-1], j+FirstOffsetNumber);
						j--; pituplen--;
						childfound++;
						break;
					}
			}

			if ( gistnospace(pages[numbuffer-1], itup, lenitup) ) { 
				/* no space left on page, so we should split */
				buffers[numbuffer] = XLogReadBuffer(true, index, P_NEW);
				if (!BufferIsValid(buffers[numbuffer]))
					elog(PANIC, "gistContinueInsert: can't create new block");
        			GISTInitBuffer(buffers[numbuffer], 0);
				pages[numbuffer] = BufferGetPage( buffers[numbuffer] );
				gistfillbuffer( index, pages[numbuffer], itup, lenitup, FirstOffsetNumber );
				numbuffer++;

				if ( BufferGetBlockNumber( buffers[0] ) == GIST_ROOT_BLKNO ) {
					IndexTuple *parentitup;

					parentitup = gistextractbuffer(buffers[numbuffer-1], &pituplen);

					/* we split root, just copy tuples from old root to new page */
					if ( i+1 != insert->pathlen )
						elog(PANIC,"gistContinueInsert: can't restore index '%s'",
							RelationGetRelationName( index ));

					/* fill new page */ 
					buffers[numbuffer] = XLogReadBuffer(true, index, P_NEW);
					if (!BufferIsValid(buffers[numbuffer]))
						elog(PANIC, "gistContinueInsert: can't create new block");
        				GISTInitBuffer(buffers[numbuffer], 0);
					pages[numbuffer] = BufferGetPage( buffers[numbuffer] );
					gistfillbuffer(index, pages[numbuffer], parentitup, pituplen, FirstOffsetNumber);
					numbuffer++;

					/* fill root page */
					GISTInitBuffer(buffers[0], 0);
					for(j=1;j<numbuffer;j++) {
						IndexTuple  tuple = gist_form_invalid_tuple( BufferGetBlockNumber( buffers[j] ) );
						if ( InvalidOffsetNumber == PageAddItem(pages[0], 
								(Item)tuple,
								IndexTupleSize( tuple ),
								(OffsetNumber)j,
								LP_USED) )
							elog( PANIC,"gistContinueInsert: can't restore index '%s'",
									RelationGetRelationName( index ));
						}
				}
			} else 
				gistfillbuffer( index, pages[numbuffer-1], itup, lenitup, 
					(PageIsEmpty(pages[numbuffer-1])) ? 
						FirstOffsetNumber : OffsetNumberNext(PageGetMaxOffsetNumber(pages[numbuffer-1])) );

			lenitup=numbuffer;
			for(j=0;j<numbuffer;j++) {
				itup[j]=gist_form_invalid_tuple( BufferGetBlockNumber( buffers[j] ) );
				PageSetLSN(pages[j], insert->lsn);
				PageSetTLI(pages[j], ThisTimeLineID);
				LockBuffer(buffers[j], BUFFER_LOCK_UNLOCK);
				WriteBuffer( buffers[j] );
			}
		}
	}

	MemoryContextSwitchTo(oldCxt);
	MemoryContextReset(opCtx);
}

void
gist_xlog_startup(void) {
	incomplete_inserts=NIL;
	insertCtx = AllocSetContextCreate(CurrentMemoryContext,
		"GiST recovery temporary context",	
                                 ALLOCSET_DEFAULT_MINSIZE,
                                 ALLOCSET_DEFAULT_INITSIZE,
                                 ALLOCSET_DEFAULT_MAXSIZE);
	opCtx = createTempGistContext();
}

void
gist_xlog_cleanup(void) {
	ListCell   *l;

	foreach(l, incomplete_inserts) {
		gistIncompleteInsert	*insert = (gistIncompleteInsert*) lfirst(l);
		gistContinueInsert(insert);
	}
	MemoryContextDelete(opCtx);
	MemoryContextDelete(insertCtx);	
}


XLogRecData *
formSplitRdata(RelFileNode node, BlockNumber blkno, 
		OffsetNumber *todelete, int ntodelete, 
		IndexTuple *itup, int ituplen, ItemPointer key, 
		BlockNumber *path, int pathlen, SplitedPageLayout *dist ) {
		
	XLogRecData     *rdata;
	gistxlogPageSplit	*xlrec = (gistxlogPageSplit*)palloc(sizeof(gistxlogPageSplit));
	SplitedPageLayout	*ptr;
	int npage = 0, cur=1, i;

	ptr=dist;
	while( ptr ) {
		npage++;
		ptr=ptr->next;
	}

	rdata = (XLogRecData*)palloc(sizeof(XLogRecData)*(npage*2 + ituplen + 3));

	xlrec->node = node;
	xlrec->origblkno = blkno;
	xlrec->npage = (uint16)npage;
	xlrec->nitup = (uint16)ituplen;
	xlrec->ntodelete = (uint16)ntodelete;
	xlrec->pathlen = (uint16)pathlen;
	if ( key )
		xlrec->key = *key;
	else
		ItemPointerSetInvalid( &(xlrec->key) );
	
	rdata[0].buffer = InvalidBuffer;
	rdata[0].data   = (char *) xlrec;
	rdata[0].len    = sizeof( gistxlogPageSplit );
	rdata[0].next   = NULL;

	if ( pathlen ) {
		rdata[cur-1].next   = &(rdata[cur]);
		rdata[cur].buffer = InvalidBuffer;
		rdata[cur].data = (char*)path;
		rdata[cur].len = MAXALIGN(sizeof(BlockNumber)*pathlen);
		rdata[cur].next = NULL;
		cur++;
	}

	if ( ntodelete ) {
		rdata[cur-1].next   = &(rdata[cur]);
		rdata[cur].buffer = InvalidBuffer;
		rdata[cur].data = (char*)todelete;
		rdata[cur].len = MAXALIGN(sizeof(OffsetNumber)*ntodelete);
		rdata[cur].next = NULL;
		cur++;
	}

	/* new tuples */
	for(i=0;i<ituplen;i++) {
		rdata[cur].buffer = InvalidBuffer;
		rdata[cur].data   = (char*)(itup[i]);
		rdata[cur].len  = IndexTupleSize(itup[i]);
		rdata[cur].next  = NULL;
		rdata[cur-1].next = &(rdata[cur]);
		cur++;
	}

	ptr=dist;
	while(ptr) {
		rdata[cur].buffer = InvalidBuffer;
		rdata[cur].data   = (char*)&(ptr->block);
		rdata[cur].len  = sizeof(gistxlogPage);
		rdata[cur-1].next = &(rdata[cur]);
		cur++;

		rdata[cur].buffer = InvalidBuffer;
		rdata[cur].data   = (char*)(ptr->list);
		rdata[cur].len    = MAXALIGN(sizeof(OffsetNumber)*ptr->block.num);
		if ( rdata[cur].len > sizeof(OffsetNumber)*ptr->block.num )
			rdata[cur].data = repalloc( rdata[cur].data, rdata[cur].len );
		rdata[cur-1].next = &(rdata[cur]);
		rdata[cur].next=NULL;
		cur++;
		ptr=ptr->next;
	}

	return rdata;	 
}


XLogRecData *
formUpdateRdata(RelFileNode node, BlockNumber blkno, 
		OffsetNumber *todelete, int ntodelete, bool emptypage,
		IndexTuple *itup, int ituplen, ItemPointer key, 
		BlockNumber *path, int pathlen) {
	XLogRecData	*rdata;
	gistxlogEntryUpdate	*xlrec = (gistxlogEntryUpdate*)palloc(sizeof(gistxlogEntryUpdate));

	xlrec->node = node;
	xlrec->blkno = blkno;
	if ( key )
		xlrec->key = *key;
	else
		ItemPointerSetInvalid( &(xlrec->key) );
	
	if ( emptypage ) {
		xlrec->isemptypage = true;
		xlrec->ntodelete = 0;
		xlrec->pathlen = 0;
		
		rdata = (XLogRecData*)palloc( sizeof(XLogRecData) );
		rdata->buffer = InvalidBuffer;
		rdata->data = (char*)xlrec;
		rdata->len = sizeof(gistxlogEntryUpdate);
		rdata->next = NULL;
	} else {
		int cur=1,i;

		xlrec->isemptypage = false;
		xlrec->ntodelete = ntodelete;
		xlrec->pathlen = pathlen;

		rdata = (XLogRecData*) palloc( sizeof(XLogRecData) * ( 3 + ituplen ) );

		rdata->buffer = InvalidBuffer;
		rdata->data = (char*)xlrec;
		rdata->len = sizeof(gistxlogEntryUpdate);
		rdata->next = NULL;

		if ( pathlen ) {
			rdata[cur-1].next   = &(rdata[cur]);
			rdata[cur].buffer = InvalidBuffer;
			rdata[cur].data = (char*)path;
			rdata[cur].len = MAXALIGN(sizeof(BlockNumber)*pathlen);
			rdata[cur].next = NULL;
			cur++;
		}

		if ( ntodelete ) {
			rdata[cur-1].next   = &(rdata[cur]);
			rdata[cur].buffer = InvalidBuffer;
			rdata[cur].data = (char*)todelete;
			rdata[cur].len = MAXALIGN(sizeof(OffsetNumber)*ntodelete);
			rdata[cur].next = NULL;
			cur++;
		}

		/* new tuples */
                for(i=0;i<ituplen;i++) {
			rdata[cur].buffer = InvalidBuffer;
			rdata[cur].data   = (char*)(itup[i]);
			rdata[cur].len  = IndexTupleSize(itup[i]);
			rdata[cur].next  = NULL;
			rdata[cur-1].next = &(rdata[cur]);
			cur++;
		}
	}

	return rdata;
}

XLogRecPtr 
gistxlogInsertCompletion(RelFileNode node, ItemPointerData *keys, int len) {
	gistxlogInsertComplete  xlrec;
	XLogRecData             rdata[2];
	XLogRecPtr recptr;

	Assert(len>0);
	xlrec.node = node;

	rdata[0].buffer = InvalidBuffer;
	rdata[0].data   = (char *) &xlrec;
	rdata[0].len    = sizeof( gistxlogInsertComplete );
	rdata[0].next   = &(rdata[1]);

	rdata[1].buffer = InvalidBuffer;
	rdata[1].data   = (char *) keys;
	rdata[1].len    = sizeof( ItemPointerData ) * len;
	rdata[1].next   = NULL;

	START_CRIT_SECTION();

	recptr = XLogInsert(RM_GIST_ID, XLOG_GIST_INSERT_COMPLETE, rdata);
  
	END_CRIT_SECTION();

	return recptr;
}
