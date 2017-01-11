/* Copyright (c) Mark Harmstone 2017
 * 
 * This file is part of WinBtrfs.
 * 
 * WinBtrfs is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public Licence as published by
 * the Free Software Foundation, either version 3 of the Licence, or
 * (at your option) any later version.
 * 
 * WinBtrfs is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public Licence for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public Licence
 * along with WinBtrfs.  If not, see <http://www.gnu.org/licenses/>. */

#include "btrfs_drv.h"

#define SCRUB_UNIT 0x100000 // 1 MB

struct _scrub_context;

typedef struct {
    struct _scrub_context* context;
    PIRP Irp;
    IO_STATUS_BLOCK iosb;
    UINT8* buf;
    BOOL csum_error;
    UINT32* bad_csums;
} scrub_context_stripe;

typedef struct _scrub_context {
    KEVENT Event;
    scrub_context_stripe* stripes;
    LONG stripes_left;
} scrub_context;

typedef struct {
    ANSI_STRING name;
    BOOL orig_subvol;
    LIST_ENTRY list_entry;
} path_part;

static void log_file_checksum_error(device_extension* Vcb, UINT64 subvol, UINT64 inode, UINT64 offset) {
    LIST_ENTRY *le, parts;
    root* r = NULL;
    KEY searchkey;
    traverse_ptr tp;
    UINT64 dir;
    BOOL orig_subvol = TRUE, not_in_tree = FALSE;
    ANSI_STRING fn;
    
    le = Vcb->roots.Flink;
    while (le != &Vcb->roots) {
        root* r2 = CONTAINING_RECORD(le, root, list_entry);
        
        if (r2->id == subvol) {
            r = r2;
            break;
        }
        
        le = le->Flink;
    }
    
    if (!r) {
        ERR("could not find subvol %llx\n", subvol);
        return;
    }
    
    InitializeListHead(&parts);
    
    dir = inode;
    
    while (TRUE) {
        NTSTATUS Status;
        
        if (dir == r->root_item.objid) {
            if (r == Vcb->root_fileref->fcb->subvol)
                break;
            
            searchkey.obj_id = r->id;
            searchkey.obj_type = TYPE_ROOT_BACKREF;
            searchkey.offset = 0xffffffffffffffff;
            
            Status = find_item(Vcb, Vcb->root_root, &tp, &searchkey, FALSE, NULL);
            if (!NT_SUCCESS(Status)) {
                ERR("find_tree returned %08x\n", Status);
                goto end;
            }
            
            if (tp.item->key.obj_id == searchkey.obj_id && tp.item->key.obj_type == searchkey.obj_type) {
                ROOT_REF* rr = (ROOT_REF*)tp.item->data;
                path_part* pp;
                
                if (tp.item->size < sizeof(ROOT_REF)) {
                    ERR("(%llx,%x,%llx) was %u bytes, expected at least %u\n", tp.item->key.obj_id, tp.item->key.obj_type, tp.item->key.offset, tp.item->size, sizeof(ROOT_REF));
                    goto end;
                }
                
                if (tp.item->size < offsetof(ROOT_REF, name[0]) + rr->n) {
                    ERR("(%llx,%x,%llx) was %u bytes, expected at least %u\n", tp.item->key.obj_id, tp.item->key.obj_type, tp.item->key.offset,
                        tp.item->size, offsetof(ROOT_REF, name[0]) + rr->n);
                    goto end;
                }
                
                pp = ExAllocatePoolWithTag(PagedPool, sizeof(path_part), ALLOC_TAG);
                if (!pp) {
                    ERR("out of memory\n");
                    goto end;
                }
                
                pp->name.Buffer = rr->name;
                pp->name.Length = pp->name.MaximumLength = rr->n;
                pp->orig_subvol = FALSE;
                
                InsertTailList(&parts, &pp->list_entry);
                
                r = NULL;
                
                le = Vcb->roots.Flink;
                while (le != &Vcb->roots) {
                    root* r2 = CONTAINING_RECORD(le, root, list_entry);
                    
                    if (r2->id == tp.item->key.offset) {
                        r = r2;
                        break;
                    }
                    
                    le = le->Flink;
                }
                
                if (!r) {
                    ERR("could not find subvol %llx\n", tp.item->key.offset);
                    goto end;
                }
                
                dir = rr->dir;
                orig_subvol = FALSE;
            } else {
                not_in_tree = TRUE;
                break;
            }
        } else {
            searchkey.obj_id = dir;
            searchkey.obj_type = TYPE_INODE_EXTREF;
            searchkey.offset = 0xffffffffffffffff;
            
            Status = find_item(Vcb, r, &tp, &searchkey, FALSE, NULL);
            if (!NT_SUCCESS(Status)) {
                ERR("find_tree returned %08x\n", Status);
                goto end;
            }
            
            if (tp.item->key.obj_id == searchkey.obj_id && tp.item->key.obj_type == TYPE_INODE_REF) {
                INODE_REF* ir = (INODE_REF*)tp.item->data;
                path_part* pp;
                
                if (tp.item->size < sizeof(INODE_REF)) {
                    ERR("(%llx,%x,%llx) was %u bytes, expected at least %u\n", tp.item->key.obj_id, tp.item->key.obj_type, tp.item->key.offset, tp.item->size, sizeof(INODE_REF));
                    goto end;
                }
                
                if (tp.item->size < offsetof(INODE_REF, name[0]) + ir->n) {
                    ERR("(%llx,%x,%llx) was %u bytes, expected at least %u\n", tp.item->key.obj_id, tp.item->key.obj_type, tp.item->key.offset,
                        tp.item->size, offsetof(INODE_REF, name[0]) + ir->n);
                    goto end;
                }
                
                pp = ExAllocatePoolWithTag(PagedPool, sizeof(path_part), ALLOC_TAG);
                if (!pp) {
                    ERR("out of memory\n");
                    goto end;
                }
                
                pp->name.Buffer = ir->name;
                pp->name.Length = pp->name.MaximumLength = ir->n;
                pp->orig_subvol = orig_subvol;
                
                InsertTailList(&parts, &pp->list_entry);
                
                if (dir == tp.item->key.offset)
                    break;
                
                dir = tp.item->key.offset;
            } else if (tp.item->key.obj_id == searchkey.obj_id && tp.item->key.obj_type == TYPE_INODE_EXTREF) {
                INODE_EXTREF* ier = (INODE_EXTREF*)tp.item->data;
                path_part* pp;

                if (tp.item->size < sizeof(INODE_EXTREF)) {
                    ERR("(%llx,%x,%llx) was %u bytes, expected at least %u\n", tp.item->key.obj_id, tp.item->key.obj_type, tp.item->key.offset,
                                                                               tp.item->size, sizeof(INODE_EXTREF));
                    goto end;
                }
                
                if (tp.item->size < offsetof(INODE_EXTREF, name[0]) + ier->n) {
                    ERR("(%llx,%x,%llx) was %u bytes, expected at least %u\n", tp.item->key.obj_id, tp.item->key.obj_type, tp.item->key.offset,
                        tp.item->size, offsetof(INODE_EXTREF, name[0]) + ier->n);
                    goto end;
                }
                
                pp = ExAllocatePoolWithTag(PagedPool, sizeof(path_part), ALLOC_TAG);
                if (!pp) {
                    ERR("out of memory\n");
                    goto end;
                }
                
                pp->name.Buffer = ier->name;
                pp->name.Length = pp->name.MaximumLength = ier->n;
                pp->orig_subvol = orig_subvol;
                
                InsertTailList(&parts, &pp->list_entry);
                
                if (dir == ier->dir)
                    break;
                
                dir = ier->dir;
            } else {
                ERR("could not find INODE_REF for inode %llx in subvol %llx\n", dir, r->id);
                goto end;
            }
        }
    }
    
    fn.MaximumLength = 0;
    
    if (not_in_tree) {
        le = parts.Blink;
        while (le != &parts) {
            path_part* pp = CONTAINING_RECORD(le, path_part, list_entry);
            LIST_ENTRY* le2 = le->Blink;
            
            if (pp->orig_subvol)
                break;
            
            RemoveTailList(&parts);
            ExFreePool(pp);
            
            le = le2;
        }
    }
    
    le = parts.Flink;
    while (le != &parts) {
        path_part* pp = CONTAINING_RECORD(le, path_part, list_entry);
        
        fn.MaximumLength += pp->name.Length + 1;
            
        le = le->Flink;
    }
    
    fn.Buffer = ExAllocatePoolWithTag(PagedPool, fn.MaximumLength, ALLOC_TAG);
    if (!fn.Buffer) {
        ERR("out of memory\n");
        goto end;
    }
    
    fn.Length = 0;
    
    le = parts.Blink;
    while (le != &parts) {
        path_part* pp = CONTAINING_RECORD(le, path_part, list_entry);
        
        fn.Buffer[fn.Length] = '\\';
        fn.Length++;
        
        RtlCopyMemory(&fn.Buffer[fn.Length], pp->name.Buffer, pp->name.Length);
        fn.Length += pp->name.Length;
            
        le = le->Blink;
    }
    
    if (not_in_tree)
        ERR("subvol %llx, %.*s, offset %llx\n", subvol, fn.Length, fn.Buffer, offset);
    else
        ERR("%.*s, offset %llx\n", fn.Length, fn.Buffer, offset);
    
    ExFreePool(fn.Buffer);
    
end:
    while (!IsListEmpty(&parts)) {
        path_part* pp = CONTAINING_RECORD(RemoveHeadList(&parts), path_part, list_entry);
        
        ExFreePool(pp);
    }
}

static void log_file_checksum_error_shared(device_extension* Vcb, UINT64 treeaddr, UINT64 addr, UINT64 extent) {
    tree_header* tree;
    NTSTATUS Status;
    leaf_node* ln;
    ULONG i;
    
    tree = ExAllocatePoolWithTag(PagedPool, Vcb->superblock.node_size, ALLOC_TAG);
    if (!tree) {
        ERR("out of memory\n");
        return;
    }
    
    Status = read_data(Vcb, treeaddr, Vcb->superblock.node_size, NULL, TRUE, (UINT8*)tree, NULL, NULL, NULL, FALSE);
    if (!NT_SUCCESS(Status)) {
        ERR("read_data returned %08x\n", Status);
        goto end;
    }
    
    ln = (leaf_node*)&tree[1];
    
    for (i = 0; i < tree->num_items; i++) {
        if (ln[i].key.obj_type == TYPE_EXTENT_DATA && ln[i].size >= sizeof(EXTENT_DATA) - 1 + sizeof(EXTENT_DATA2)) {
            EXTENT_DATA* ed = (EXTENT_DATA*)((UINT8*)tree + sizeof(tree_header) + ln[i].offset);
            EXTENT_DATA2* ed2 = (EXTENT_DATA2*)ed->data;

            if (ed->type == EXTENT_TYPE_REGULAR && ed2->size != 0 && ed2->address == addr)
                log_file_checksum_error(Vcb, tree->tree_id, ln[i].key.obj_id, ln[i].key.offset + addr - extent);
        }
    }
    
end:
    ExFreePool(tree);
}

static void log_tree_checksum_error(device_extension* Vcb, UINT64 root, UINT8 level, KEY* firstitem) {
    if (firstitem) {
        ERR("root %llx, level %u, first item (%llx,%x,%llx)\n", root, level, firstitem->obj_id,
                                                                firstitem->obj_type, firstitem->offset);
    } else
        ERR("root %llx, level %u\n", root, level);
}

static void log_tree_checksum_error_shared(device_extension* Vcb, UINT64 offset, UINT64 address) {
    tree_header* tree;
    NTSTATUS Status;
    internal_node* in;
    ULONG i;
    
    tree = ExAllocatePoolWithTag(PagedPool, Vcb->superblock.node_size, ALLOC_TAG);
    if (!tree) {
        ERR("out of memory\n");
        return;
    }
    
    Status = read_data(Vcb, offset, Vcb->superblock.node_size, NULL, TRUE, (UINT8*)tree, NULL, NULL, NULL, FALSE);
    if (!NT_SUCCESS(Status)) {
        ERR("read_data returned %08x\n", Status);
        goto end;
    }
    
    if (tree->level == 0) {
        ERR("tree level was 0\n");
        goto end;
    }
    
    in = (internal_node*)&tree[1];
    
    for (i = 0; i < tree->num_items; i++) {
        if (in[i].address == address) {
            log_tree_checksum_error(Vcb, tree->tree_id, tree->level - 1, &in[i].key);
            break;
        }
    }
    
end:
    ExFreePool(tree);
}

static void log_unrecoverable_error(device_extension* Vcb, UINT64 address) {
    KEY searchkey;
    traverse_ptr tp;
    NTSTATUS Status;
    EXTENT_ITEM* ei;
    EXTENT_ITEM2* ei2 = NULL;
    UINT8* ptr;
    ULONG len;
    UINT64 rc;
    
    // FIXME - make log accessible from userspace
    // FIXME - still log even if rest of this function fails
    
    searchkey.obj_id = address;
    searchkey.obj_type = TYPE_METADATA_ITEM;
    searchkey.offset = 0xffffffffffffffff;
    
    Status = find_item(Vcb, Vcb->extent_root, &tp, &searchkey, FALSE, NULL);
    if (!NT_SUCCESS(Status)) {
        ERR("find_tree returned %08x\n", Status);
        return;
    }
    
    if ((tp.item->key.obj_type != TYPE_EXTENT_ITEM && tp.item->key.obj_type != TYPE_METADATA_ITEM) ||
        tp.item->key.obj_id >= address + Vcb->superblock.sector_size ||
        (tp.item->key.obj_type == TYPE_EXTENT_ITEM && tp.item->key.obj_id + tp.item->key.offset <= address) ||
        (tp.item->key.obj_type == TYPE_METADATA_ITEM && tp.item->key.obj_id + Vcb->superblock.node_size <= address)
    )
        return;
    
    if (tp.item->size < sizeof(EXTENT_ITEM)) {
        ERR("(%llx,%x,%llx) was %u bytes, expected at least %u\n", tp.item->key.obj_id, tp.item->key.obj_type, tp.item->key.offset, tp.item->size, sizeof(EXTENT_ITEM));
        return;
    }
    
    ei = (EXTENT_ITEM*)tp.item->data;
    ptr = (UINT8*)&ei[1];
    len = tp.item->size - sizeof(EXTENT_ITEM);
    
    if (tp.item->key.obj_id == TYPE_EXTENT_ITEM && ei->flags & EXTENT_ITEM_TREE_BLOCK) {
        if (tp.item->size < sizeof(EXTENT_ITEM) + sizeof(EXTENT_ITEM2)) {
            ERR("(%llx,%x,%llx) was %u bytes, expected at least %u\n", tp.item->key.obj_id, tp.item->key.obj_type, tp.item->key.offset,
                                                                       tp.item->size, sizeof(EXTENT_ITEM) + sizeof(EXTENT_ITEM2));
            return;
        }
        
        ei2 = (EXTENT_ITEM2*)ptr;
        
        ptr += sizeof(EXTENT_ITEM2);
        len -= sizeof(EXTENT_ITEM2);
    }
    
    rc = 0;
    
    while (len > 0) {
        UINT8 type = *ptr;
        
        ptr++;
        len--;
        
        if (type == TYPE_TREE_BLOCK_REF) {
            TREE_BLOCK_REF* tbr;
            
            if (len < sizeof(TREE_BLOCK_REF)) {
                ERR("TREE_BLOCK_REF takes up %u bytes, but only %u remaining\n", sizeof(TREE_BLOCK_REF), len);
                break;
            }
            
            tbr = (TREE_BLOCK_REF*)ptr;
            
            log_tree_checksum_error(Vcb, tbr->offset, ei2 ? ei2->level : tp.item->key.offset, ei2 ? &ei2->firstitem : NULL);
            
            rc++;
            
            ptr += sizeof(TREE_BLOCK_REF);
            len -= sizeof(TREE_BLOCK_REF);
        } else if (type == TYPE_EXTENT_DATA_REF) {
            EXTENT_DATA_REF* edr;
            
            if (len < sizeof(EXTENT_DATA_REF)) {
                ERR("EXTENT_DATA_REF takes up %u bytes, but only %u remaining\n", sizeof(EXTENT_DATA_REF), len);
                break;
            }
            
            edr = (EXTENT_DATA_REF*)ptr;
            
            log_file_checksum_error(Vcb, edr->root, edr->objid, edr->offset + address - tp.item->key.obj_id);
            
            rc += edr->count;
            
            ptr += sizeof(EXTENT_DATA_REF);
            len -= sizeof(EXTENT_DATA_REF);
        } else if (type == TYPE_SHARED_BLOCK_REF) {
            SHARED_BLOCK_REF* sbr;
            
            if (len < sizeof(SHARED_BLOCK_REF)) {
                ERR("SHARED_BLOCK_REF takes up %u bytes, but only %u remaining\n", sizeof(SHARED_BLOCK_REF), len);
                break;
            }
            
            sbr = (SHARED_BLOCK_REF*)ptr;
            
            log_tree_checksum_error_shared(Vcb, sbr->offset, address);
            
            rc++;
            
            ptr += sizeof(SHARED_BLOCK_REF);
            len -= sizeof(SHARED_BLOCK_REF);
        } else if (type == TYPE_SHARED_DATA_REF) {
            SHARED_DATA_REF* sdr;
            
            if (len < sizeof(SHARED_DATA_REF)) {
                ERR("SHARED_DATA_REF takes up %u bytes, but only %u remaining\n", sizeof(SHARED_DATA_REF), len);
                break;
            }
            
            sdr = (SHARED_DATA_REF*)ptr;
            
            log_file_checksum_error_shared(Vcb, sdr->offset, address, tp.item->key.obj_id);
            
            rc += sdr->count;
            
            ptr += sizeof(SHARED_DATA_REF);
            len -= sizeof(SHARED_DATA_REF);
        } else {
            ERR("unknown extent type %x\n", type);
            break;
        }
    }
    
    if (rc < ei->refcount) {
        // FIXME - non-inline extents
    }
}

static NTSTATUS STDCALL scrub_read_completion(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID conptr) {
    scrub_context_stripe* stripe = conptr;
    scrub_context* context = (scrub_context*)stripe->context;
    ULONG left = InterlockedDecrement(&context->stripes_left);
    
    stripe->iosb = Irp->IoStatus;
    
    if (left == 0)
        KeSetEvent(&context->Event, 0, FALSE);
    
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS scrub_extent_dup(device_extension* Vcb, chunk* c, UINT64 offset, UINT64 size, UINT32* csum) {
    ULONG i;
    scrub_context* context = NULL;
    CHUNK_ITEM_STRIPE* cis;
    NTSTATUS Status;
    BOOL csum_error = FALSE;
    
    // FIXME - make work when degraded
    
    TRACE("(%p, %p, %llx, %llx, %p)\n", Vcb, c, offset, size, csum);
    
    context = ExAllocatePoolWithTag(NonPagedPool, sizeof(scrub_context), ALLOC_TAG);
    if (!context) {
        ERR("out of memory\n");
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto end;
    }
    
    context->stripes = ExAllocatePoolWithTag(NonPagedPool, sizeof(scrub_context_stripe) * c->chunk_item->num_stripes, ALLOC_TAG);
    if (!context) {
        ERR("out of memory\n");
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto end;
    }
    
    RtlZeroMemory(context->stripes, sizeof(scrub_context_stripe) * c->chunk_item->num_stripes);
    
    cis = (CHUNK_ITEM_STRIPE*)&c->chunk_item[1];
    
    for (i = 0; i < c->chunk_item->num_stripes; i++) {
        PIO_STACK_LOCATION IrpSp;
        
        context->stripes[i].context = (struct _scrub_context*)context;
        context->stripes[i].buf = ExAllocatePoolWithTag(NonPagedPool, size, ALLOC_TAG);
        
        if (!context->stripes[i].buf) {
            ERR("out of memory\n");
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto end;
        }
        
        context->stripes[i].Irp = IoAllocateIrp(c->devices[i]->devobj->StackSize, FALSE);
        
        if (!context->stripes[i].Irp) {
            ERR("IoAllocateIrp failed\n");
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto end;
        }
        
        IrpSp = IoGetNextIrpStackLocation(context->stripes[i].Irp);
        IrpSp->MajorFunction = IRP_MJ_READ;
        
        if (c->devices[i]->devobj->Flags & DO_BUFFERED_IO)
            FIXME("FIXME - buffered IO\n");
        else if (c->devices[i]->devobj->Flags & DO_DIRECT_IO) {
            context->stripes[i].Irp->MdlAddress = IoAllocateMdl(context->stripes[i].buf, size, FALSE, FALSE, NULL);
            if (!context->stripes[i].Irp->MdlAddress) {
                ERR("IoAllocateMdl failed\n");
                Status = STATUS_INSUFFICIENT_RESOURCES;
                goto end;
            }
            
            MmProbeAndLockPages(context->stripes[i].Irp->MdlAddress, KernelMode, IoWriteAccess);
        } else
            context->stripes[i].Irp->UserBuffer = context->stripes[i].buf;

        IrpSp->Parameters.Read.Length = size;
        IrpSp->Parameters.Read.ByteOffset.QuadPart = c->devices[i]->offset + offset - c->offset + cis[i].offset;
        
        context->stripes[i].Irp->UserIosb = &context->stripes[i].iosb;
        
        IoSetCompletionRoutine(context->stripes[i].Irp, scrub_read_completion, &context->stripes[i], TRUE, TRUE, TRUE);
    }
    
    context->stripes_left = c->chunk_item->num_stripes;
    
    KeInitializeEvent(&context->Event, NotificationEvent, FALSE);
    
    for (i = 0; i < c->chunk_item->num_stripes; i++) {
        IoCallDriver(c->devices[i]->devobj, context->stripes[i].Irp);
    }
    
    KeWaitForSingleObject(&context->Event, Executive, KernelMode, FALSE, NULL);
    
    // return an error if any of the stripes returned an error
    for (i = 0; i < c->chunk_item->num_stripes; i++) {
        if (!NT_SUCCESS(context->stripes[i].iosb.Status)) {
            Status = context->stripes[i].iosb.Status;
            goto end;
        }
    }
    
    // FIXME - if first stripe is okay, we only need to check that the others are identical to it
    
    if (csum) {
        for (i = 0; i < c->chunk_item->num_stripes; i++) {
            Status = check_csum(Vcb, context->stripes[i].buf, size / Vcb->superblock.sector_size, csum);
            if (Status == STATUS_CRC_ERROR) {
                context->stripes[i].csum_error = TRUE;
                csum_error = TRUE;
            } else if (!NT_SUCCESS(Status)) {
                ERR("check_csum returned %08x\n", Status);
                goto end;
            }
        }
    } else {
        for (i = 0; i < c->chunk_item->num_stripes; i++) {
            ULONG j;
            
            for (j = 0; j < size / Vcb->superblock.node_size; j++) {
                tree_header* th = (tree_header*)&context->stripes[i].buf[j * Vcb->superblock.node_size];
                UINT32 crc32 = ~calc_crc32c(0xffffffff, (UINT8*)&th->fs_uuid, Vcb->superblock.node_size - sizeof(th->csum));
                
                // FIXME - check tree address is what was expected
            
                if (crc32 != *((UINT32*)th->csum)) {
                    context->stripes[i].csum_error = TRUE;
                    csum_error = TRUE;
                }
            }
        }
    }
    
    if (!csum_error) {
        Status = STATUS_SUCCESS;
        goto end;
    }
    
    // handle checksum error
    
    for (i = 0; i < c->chunk_item->num_stripes; i++) {
        if (context->stripes[i].csum_error) {
            if (csum) {
                context->stripes[i].bad_csums = ExAllocatePoolWithTag(PagedPool, size * sizeof(UINT32) / Vcb->superblock.sector_size, ALLOC_TAG);
                if (!context->stripes[i].bad_csums) {
                    ERR("out of memory\n");
                    Status = STATUS_INSUFFICIENT_RESOURCES;
                    goto end;
                }
                
                Status = calc_csum(Vcb, context->stripes[i].buf, size / Vcb->superblock.sector_size, context->stripes[i].bad_csums);
                if (!NT_SUCCESS(Status)) {
                    ERR("calc_csum returned %08x\n", Status);
                    goto end;
                }
            } else {
                ULONG j;
                
                context->stripes[i].bad_csums = ExAllocatePoolWithTag(PagedPool, size * sizeof(UINT32) / Vcb->superblock.node_size, ALLOC_TAG);
                if (!context->stripes[i].bad_csums) {
                    ERR("out of memory\n");
                    Status = STATUS_INSUFFICIENT_RESOURCES;
                    goto end;
                }
                
                for (j = 0; j < size / Vcb->superblock.node_size; j++) {
                    tree_header* th = (tree_header*)&context->stripes[i].buf[j * Vcb->superblock.node_size];
                    UINT32 crc32 = ~calc_crc32c(0xffffffff, (UINT8*)&th->fs_uuid, Vcb->superblock.node_size - sizeof(th->csum));
                    
                    context->stripes[i].bad_csums[j] = crc32;
                }
            }
        }
    }
    
    if (c->chunk_item->num_stripes > 1) {
        ULONG good_stripe = 0xffffffff;
        
        for (i = 0; i < c->chunk_item->num_stripes; i++) {
            if (!context->stripes[i].csum_error) {
                good_stripe = i;
                break;
            }
        }
        
        if (good_stripe != 0xffffffff) {
            // log
            
            for (i = 0; i < c->chunk_item->num_stripes; i++) {
                if (context->stripes[i].csum_error) {
                    ULONG j;
                    
                    if (csum) {
                        for (j = 0; j < size / Vcb->superblock.sector_size; j++) {
                            if (context->stripes[i].bad_csums[j] != csum[j]) {
                                UINT64 addr = offset + UInt32x32To64(j, Vcb->superblock.sector_size);
                                
                                ERR("recovering from data checksum error at %llx on device %llx\n", addr, c->devices[i]->devitem.dev_id);
                            }
                        }
                    } else {
                        for (j = 0; j < size / Vcb->superblock.node_size; j++) {
                            tree_header* th = (tree_header*)&context->stripes[i].buf[j * Vcb->superblock.node_size];
                            
                            if (context->stripes[i].bad_csums[j] != *((UINT32*)th->csum)) {
                                UINT64 addr = offset + UInt32x32To64(j, Vcb->superblock.node_size);
                                
                                ERR("recovering from metadata checksum error at %llx on device %llx\n", addr, c->devices[i]->devitem.dev_id);
                            }
                        }
                    }
                }
            }
            
            // write good data over bad
            
            for (i = 0; i < c->chunk_item->num_stripes; i++) {
                if (context->stripes[i].csum_error) {
                    Status = write_data_phys(c->devices[i]->devobj, c->devices[i]->offset + cis[i].offset + offset - c->offset,
                                             context->stripes[good_stripe].buf, size);
                    
                    if (!NT_SUCCESS(Status)) {
                        ERR("write_data_phys returned %08x\n", Status);
                        goto end;
                    }
                }
            }
            
            Status = STATUS_SUCCESS;
            goto end;
        }
        
        // if csum errors on all stripes, check sector by sector
        
        for (i = 0; i < c->chunk_item->num_stripes; i++) {
            ULONG j;
            
            if (csum) {
                for (j = 0; j < size / Vcb->superblock.sector_size; j++) {
                    if (context->stripes[i].bad_csums[j] != csum[j]) {
                        ULONG k;
                        UINT64 addr = offset + UInt32x32To64(j, Vcb->superblock.sector_size);
                        BOOL recovered = FALSE;
                        
                        for (k = 0; k < c->chunk_item->num_stripes; k++) {
                            if (i != k && context->stripes[k].bad_csums[j] == csum[j]) {
                                ERR("recovering from data checksum error at %llx on device %llx\n", addr, c->devices[i]->devitem.dev_id);
                                
                                RtlCopyMemory(context->stripes[i].buf + (j * Vcb->superblock.sector_size),
                                            context->stripes[k].buf + (j * Vcb->superblock.sector_size), Vcb->superblock.sector_size);
                                
                                recovered = TRUE;
                                break;
                            }
                        }
                        
                        if (!recovered) {
                            ERR("unrecoverable data checksum error at %llx: csum was %08x, expected %08x\n", addr,
                                context->stripes[i].bad_csums[j], csum[j]);
                            
                            // FIXME - blank sector
                            
                            log_unrecoverable_error(Vcb, addr);
                        }
                    }
                }
            } else {
                for (j = 0; j < size / Vcb->superblock.node_size; j++) {
                    tree_header* th = (tree_header*)&context->stripes[i].buf[j * Vcb->superblock.node_size];
                    
                    if (context->stripes[i].bad_csums[j] != *((UINT32*)th->csum)) {
                        ULONG k;
                        UINT64 addr = offset + UInt32x32To64(j, Vcb->superblock.node_size);
                        BOOL recovered = FALSE;
                        
                        for (k = 0; k < c->chunk_item->num_stripes; k++) {
                            if (i != k) {
                                tree_header* th2 = (tree_header*)&context->stripes[k].buf[j * Vcb->superblock.node_size];
                                
                                if (context->stripes[k].bad_csums[j] == *((UINT32*)th2->csum)) {
                                    ERR("recovering from metadata checksum error at %llx on device %llx\n", addr, c->devices[i]->devitem.dev_id);
                                    
                                    RtlCopyMemory(th, th2, Vcb->superblock.node_size);
                                    
                                    recovered = TRUE;
                                    break;
                                }
                            }
                        }
                        
                        if (!recovered) {
                            ERR("unrecoverable metadata checksum error at %llx: csum was %08x, expected %08x\n", addr,
                                context->stripes[i].bad_csums[j], *((UINT32*)th->csum));
                            
                            // FIXME - blank tree
                            
                            log_unrecoverable_error(Vcb, addr);
                        }
                    }
                }
            }
        }
                   
        // write good data over bad
        
        for (i = 0; i < c->chunk_item->num_stripes; i++) {
            Status = write_data_phys(c->devices[i]->devobj, c->devices[i]->offset + cis[i].offset + offset - c->offset,
                                        context->stripes[i].buf, size);
            if (!NT_SUCCESS(Status)) {
                ERR("write_data_phys returned %08x\n", Status);
                goto end;
            }
        }
        
        Status = STATUS_SUCCESS;
        goto end;
    }
    
    for (i = 0; i < c->chunk_item->num_stripes; i++) {
        ULONG j;
        
        if (csum) {
            for (j = 0; j < size / Vcb->superblock.sector_size; j++) {
                if (context->stripes[i].bad_csums[j] != csum[j]) {
                    UINT64 addr = offset + UInt32x32To64(j, Vcb->superblock.sector_size);
                    
                    ERR("unrecoverable data checksum error at %llx: csum was %08x, expected %08x\n", addr,
                        context->stripes[i].bad_csums[j], csum[j]);
                    
                    // FIXME - blank sector
                    
                    log_unrecoverable_error(Vcb, addr);
                }
            }
        } else {
            for (j = 0; j < size / Vcb->superblock.node_size; j++) {
                tree_header* th = (tree_header*)&context->stripes[i].buf[j * Vcb->superblock.node_size];
                
                if (context->stripes[i].bad_csums[j] != *((UINT32*)th->csum)) {
                    UINT64 addr = offset + UInt32x32To64(j, Vcb->superblock.node_size);
                    
                    ERR("unrecoverable metadata checksum error at %llx: csum was %08x, expected %08x\n", addr,
                        context->stripes[i].bad_csums[j], *((UINT32*)th->csum));
                    
                    // FIXME - blank tree
                    
                    log_unrecoverable_error(Vcb, addr);
                }
            }
        }
    }
    
    Status = STATUS_SUCCESS;
    
end:
    if (context) {
        if (context->stripes) {
            for (i = 0; i < c->chunk_item->num_stripes; i++) {
                if (context->stripes[i].Irp) {
                    if (c->devices[i]->devobj->Flags & DO_DIRECT_IO && context->stripes[i].Irp->MdlAddress) {
                        MmUnlockPages(context->stripes[i].Irp->MdlAddress);
                        IoFreeMdl(context->stripes[i].Irp->MdlAddress);
                    }
                    IoFreeIrp(context->stripes[i].Irp);
                }
                
                if (context->stripes[i].buf)
                    ExFreePool(context->stripes[i].buf);
                
                if (context->stripes[i].bad_csums)
                    ExFreePool(context->stripes[i].bad_csums);
            }
            
            ExFreePool(context->stripes);
        }
            
        ExFreePool(context);
    }

    return Status;
}

static NTSTATUS scrub_data_extent(device_extension* Vcb, chunk* c, UINT64 offset, UINT64 size, ULONG type, UINT32* csum, RTL_BITMAP* bmp) {
    NTSTATUS Status;
    ULONG runlength, index;
    
    runlength = RtlFindFirstRunClear(bmp, &index);
        
    while (runlength != 0) {
        do {
            ULONG rl;
            
            if (runlength * Vcb->superblock.sector_size > SCRUB_UNIT)
                rl = SCRUB_UNIT / Vcb->superblock.sector_size;
            else
                rl = runlength;
            
            if (type == BLOCK_FLAG_DUPLICATE) {
                Status = scrub_extent_dup(Vcb, c, offset + UInt32x32To64(index, Vcb->superblock.sector_size), rl * Vcb->superblock.sector_size, &csum[index]);
                if (!NT_SUCCESS(Status)) {
                    ERR("scrub_data_extent_dup returned %08x\n", Status);
                    return Status;
                }
            }

            runlength -= rl;
            index += rl;
        } while (runlength > 0);
        
        runlength = RtlFindNextForwardRunClear(bmp, index, &index);
    }
    
    return STATUS_SUCCESS;
}

static NTSTATUS scrub_tree_run(device_extension* Vcb, chunk* c, UINT8 type, UINT64 start, UINT64 length) {
    NTSTATUS Status;
    
    TRACE("(%p, %p, %llx, %llx)\n", Vcb, c, start, length);
    
    if (type == BLOCK_FLAG_DUPLICATE) {
        Status = scrub_extent_dup(Vcb, c, start, length, NULL);
        if (!NT_SUCCESS(Status)) {
            ERR("scrub_data_extent_dup returned %08x\n", Status);
            return Status;
        }
    }
    
    return STATUS_SUCCESS;
}

static NTSTATUS scrub_chunk(device_extension* Vcb, chunk* c, UINT64* offset, BOOL* changed) {
    NTSTATUS Status;
    KEY searchkey;
    traverse_ptr tp;
    BOOL b = FALSE, tree_run = FALSE;
    ULONG type, num_extents = 0;
    UINT64 total_data = 0, tree_run_start, tree_run_end;
    
    TRACE("chunk %llx\n", c->offset);
    
    ExAcquireResourceSharedLite(&Vcb->tree_lock, TRUE);
    
    if (c->chunk_item->type & BLOCK_FLAG_DUPLICATE)
        type = BLOCK_FLAG_DUPLICATE;
    else if (c->chunk_item->type & BLOCK_FLAG_RAID0) {
        type = BLOCK_FLAG_RAID0;
        ERR("RAID0 not yet supported\n");
        goto end;
    } else if (c->chunk_item->type & BLOCK_FLAG_RAID1)
        type = BLOCK_FLAG_DUPLICATE;
    else if (c->chunk_item->type & BLOCK_FLAG_RAID10) {
        type = BLOCK_FLAG_RAID10;
        ERR("RAID10 not yet supported\n");
        goto end;
    } else if (c->chunk_item->type & BLOCK_FLAG_RAID5) {
        type = BLOCK_FLAG_RAID5;
        ERR("RAID5 not yet supported\n");
        goto end;
    } else if (c->chunk_item->type & BLOCK_FLAG_RAID6) {
        type = BLOCK_FLAG_RAID6;
        ERR("RAID6 not yet supported\n");
        goto end;
    } else // SINGLE
        type = BLOCK_FLAG_DUPLICATE;
    
    searchkey.obj_id = *offset;
    searchkey.obj_type = TYPE_METADATA_ITEM;
    searchkey.offset = 0xffffffffffffffff;
    
    Status = find_item(Vcb, Vcb->extent_root, &tp, &searchkey, FALSE, NULL);
    if (!NT_SUCCESS(Status)) {
        ERR("error - find_tree returned %08x\n", Status);
        goto end;
    }
    
    do {
        traverse_ptr next_tp;
        
        if (tp.item->key.obj_id >= c->offset + c->chunk_item->size)
            break;
        
        if (tp.item->key.obj_id >= *offset && (tp.item->key.obj_type == TYPE_EXTENT_ITEM || tp.item->key.obj_type == TYPE_METADATA_ITEM)) {
            UINT64 size = tp.item->key.obj_type == TYPE_METADATA_ITEM ? Vcb->superblock.node_size : tp.item->key.offset;
            BOOL is_tree;
            UINT32* csum = NULL;
            RTL_BITMAP bmp;
            ULONG* bmparr = NULL;
            
            TRACE("%llx\n", tp.item->key.obj_id);
            
            is_tree = FALSE;
            
            if (tp.item->key.obj_type == TYPE_METADATA_ITEM)
                is_tree = TRUE;
            else {
                EXTENT_ITEM* ei = (EXTENT_ITEM*)tp.item->data;
                
                if (tp.item->size < sizeof(EXTENT_ITEM)) {
                    ERR("(%llx,%x,%llx) was %u bytes, expected at least %u\n", tp.item->key.obj_id, tp.item->key.obj_type, tp.item->key.offset, tp.item->size, sizeof(EXTENT_ITEM));
                    Status = STATUS_INTERNAL_ERROR;
                    goto end;
                }
                
                if (ei->flags & EXTENT_ITEM_TREE_BLOCK)
                    is_tree = TRUE;
            }
            
            if (size < Vcb->superblock.sector_size) {
                ERR("extent %llx has size less than sector_size (%llx < %x)\n", tp.item->key.obj_id, Vcb->superblock.sector_size);
                Status = STATUS_INTERNAL_ERROR;
                goto end;
            }
            
            // load csum
            if (!is_tree) {
                traverse_ptr tp2;
                
                csum = ExAllocatePoolWithTag(PagedPool, sizeof(UINT32) * size / Vcb->superblock.sector_size, ALLOC_TAG);
                if (!csum) {
                    ERR("out of memory\n");
                    Status = STATUS_INSUFFICIENT_RESOURCES;
                    goto end;
                }
                
                bmparr = ExAllocatePoolWithTag(PagedPool, sector_align(((size / Vcb->superblock.sector_size) >> 3) + 1, sizeof(ULONG)), ALLOC_TAG);
                if (!bmparr) {
                    ERR("out of memory\n");
                    ExFreePool(csum);
                    Status = STATUS_INSUFFICIENT_RESOURCES;
                    goto end;
                }
                    
                RtlInitializeBitMap(&bmp, bmparr, size / Vcb->superblock.sector_size);
                RtlSetAllBits(&bmp); // 1 = no csum, 0 = csum
                
                searchkey.obj_id = EXTENT_CSUM_ID;
                searchkey.obj_type = TYPE_EXTENT_CSUM;
                searchkey.offset = tp.item->key.obj_id;
                
                Status = find_item(Vcb, Vcb->checksum_root, &tp2, &searchkey, FALSE, NULL);
                if (!NT_SUCCESS(Status) && Status != STATUS_NOT_FOUND) {
                    ERR("find_tree returned %08x\n", Status);
                    ExFreePool(csum);
                    ExFreePool(bmparr);
                    goto end;
                }
                
                if (Status != STATUS_NOT_FOUND) {
                    do {
                        traverse_ptr next_tp2;
                        
                        if (tp2.item->key.obj_type == TYPE_EXTENT_CSUM) {
                            if (tp2.item->key.offset >= tp.item->key.obj_id + size)
                                break;
                            else if (tp2.item->size >= sizeof(UINT32) && tp2.item->key.offset + (tp2.item->size * Vcb->superblock.sector_size / sizeof(UINT32)) >= tp.item->key.obj_id) {
                                UINT64 cs = max(tp.item->key.obj_id, tp2.item->key.offset);
                                UINT64 ce = min(tp.item->key.obj_id + size, tp2.item->key.offset + (tp2.item->size * Vcb->superblock.sector_size / sizeof(UINT32)));
                                
                                RtlCopyMemory(csum + ((cs - tp.item->key.obj_id) / Vcb->superblock.sector_size),
                                              tp2.item->data + ((cs - tp2.item->key.offset) * sizeof(UINT32) / Vcb->superblock.sector_size),
                                              (ce - cs) * sizeof(UINT32) / Vcb->superblock.sector_size);
                                
                                RtlClearBits(&bmp, (cs - tp.item->key.obj_id) / Vcb->superblock.sector_size, (ce - cs) / Vcb->superblock.sector_size);
                                
                                if (ce == tp.item->key.obj_id + size)
                                    break;
                            }
                        }
                        
                        if (find_next_item(Vcb, &tp2, &next_tp2, FALSE, NULL))
                            tp2 = next_tp2;
                        else
                            break;
                    } while (TRUE);
                }
            }
            
            if (tree_run) {
                if (!is_tree || tp.item->key.obj_id > tree_run_end) {
                    Status = scrub_tree_run(Vcb, c, type, tree_run_start, tree_run_end - tree_run_start);
                    if (!NT_SUCCESS(Status)) {
                        ERR("scrub_tree_run returned %08x\n", Status);
                        goto end;
                    }
                    
                    if (!is_tree)
                        tree_run = FALSE;
                    else {
                        tree_run_start = tp.item->key.obj_id;
                        tree_run_end = tp.item->key.obj_id + Vcb->superblock.node_size;
                    }
                } else
                    tree_run_end = tp.item->key.obj_id + Vcb->superblock.node_size;
            } else if (is_tree) {
                tree_run = TRUE;
                tree_run_start = tp.item->key.obj_id;
                tree_run_end = tp.item->key.obj_id + Vcb->superblock.node_size;
            }
            
            if (!is_tree) {
                Status = scrub_data_extent(Vcb, c, tp.item->key.obj_id, size, type, csum, &bmp);
                if (!NT_SUCCESS(Status)) {
                    ERR("scrub_data_extent returned %08x\n", Status);
                    goto end;
                }
            }
            
            if (!is_tree) {
                ExFreePool(csum);
                ExFreePool(bmparr);
            }
            
            *offset = tp.item->key.obj_id + size;
            *changed = TRUE;
            
            total_data += size;
            num_extents++;
            
            // only do so much at a time
            if (num_extents >= 64 || total_data >= 0x8000000) // 128 MB
                break;
        }
        
        b = find_next_item(Vcb, &tp, &next_tp, FALSE, NULL);
        
        if (b)
            tp = next_tp;
    } while (b);
    
    if (tree_run) {
        Status = scrub_tree_run(Vcb, c, type, tree_run_start, tree_run_end - tree_run_start);
        if (!NT_SUCCESS(Status)) {
            ERR("scrub_tree_run returned %08x\n", Status);
            goto end;
        }
    }
    
    Status = STATUS_SUCCESS;
    
end:
    ExReleaseResourceLite(&Vcb->tree_lock);
    
    return Status;
}

static void scrub_thread(void* context) {
    device_extension* Vcb = context;
    LIST_ENTRY rollback, chunks, *le;
    NTSTATUS Status;
    
    InitializeListHead(&rollback);
    InitializeListHead(&chunks);
    
    ExAcquireResourceExclusiveLite(&Vcb->tree_lock, TRUE);

    if (Vcb->need_write && !Vcb->readonly)
        do_write(Vcb, NULL, &rollback);
    
    free_trees(Vcb);
    
    clear_rollback(Vcb, &rollback);
    
    ExConvertExclusiveToSharedLite(&Vcb->tree_lock);
    
    ExAcquireResourceSharedLite(&Vcb->chunk_lock, TRUE);
    
    le = Vcb->chunks.Flink;
    while (le != &Vcb->chunks) {
        chunk* c = CONTAINING_RECORD(le, chunk, list_entry);
        
        ExAcquireResourceExclusiveLite(&c->lock, TRUE);
               
        if (!c->readonly)
            InsertTailList(&chunks, &c->list_entry_balance);

        ExReleaseResourceLite(&c->lock);
        
        le = le->Flink;
    }
    
    ExReleaseResourceLite(&Vcb->chunk_lock);

    ExReleaseResourceLite(&Vcb->tree_lock);
    
    while (!IsListEmpty(&chunks)) {
        chunk* c = CONTAINING_RECORD(RemoveHeadList(&chunks), chunk, list_entry_balance);
        UINT64 offset = c->offset;
        BOOL changed;
        
        c->reloc = TRUE;
        
        if (!Vcb->scrub.stopping) {
            do {
                changed = FALSE;
                
                Status = scrub_chunk(Vcb, c, &offset, &changed);
                if (!NT_SUCCESS(Status)) {
                    ERR("scrub_chunk returned %08x\n", Status);
                    Vcb->scrub.stopping = TRUE;
                    break;
                }
                
                if (offset == c->offset + c->chunk_item->size)
                    break;
            } while (changed);
        }
        
        c->reloc = FALSE;
        c->list_entry_balance.Flink = NULL;
    }
    
    ZwClose(Vcb->scrub.thread);
    Vcb->scrub.thread = NULL;
}

NTSTATUS start_scrub(device_extension* Vcb) {
    NTSTATUS Status;
    
    if (Vcb->balance.thread) {
        WARN("cannot start scrub while balance running\n");
        return STATUS_DEVICE_NOT_READY;
    }
    
    if (Vcb->scrub.thread) {
        WARN("scrub already running\n");
        return STATUS_DEVICE_NOT_READY;
    }
    
    if (Vcb->readonly)
        return STATUS_MEDIA_WRITE_PROTECTED;
    
    Vcb->scrub.stopping = FALSE;
    
    Status = PsCreateSystemThread(&Vcb->scrub.thread, 0, NULL, NULL, NULL, scrub_thread, Vcb);
    if (!NT_SUCCESS(Status)) {
        ERR("PsCreateSystemThread returned %08x\n", Status);
        return Status;
    }
    
    return STATUS_SUCCESS;
}