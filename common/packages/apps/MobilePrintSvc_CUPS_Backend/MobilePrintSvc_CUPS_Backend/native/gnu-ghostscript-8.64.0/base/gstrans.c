/* Copyright (C) 2001-2006 Artifex Software, Inc.
   All Rights Reserved.
  
  This file is part of GNU ghostscript

  GNU ghostscript is free software; you can redistribute it and/or
  modify it under the terms of the version 2 of the GNU General Public
  License as published by the Free Software Foundation.

  GNU ghostscript is distributed in the hope that it will be useful, but WITHOUT
  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
  FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along with
  ghostscript; see the file COPYING. If not, write to the Free Software Foundation,
  Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.

*/

/* $Id: gstrans.c,v 1.13 2009/04/19 13:54:27 Arabidopsis Exp $ */
/* Implementation of transparency, other than rendering */
#include "math_.h"
#include "memory_.h"
#include "gx.h"
#include "gserrors.h"
#include "gstrans.h"
#include "gsutil.h"
#include "gzstate.h"
#include "gxdevcli.h"
#include "gdevdevn.h"
#include "gxblend.h"
#include "gdevp14.h"

#define PUSH_TS 0

/* ------ Transparency-related graphics state elements ------ */

int
gs_setblendmode(gs_state *pgs, gs_blend_mode_t mode)
{
#ifdef DEBUG
    if (gs_debug_c('v')) {
	static const char *const bm_names[] = { GS_BLEND_MODE_NAMES };

	dlprintf1("[v](0x%lx)blend_mode = ", (ulong)pgs);
	if (mode >= 0 && mode < countof(bm_names))
	    dprintf1("%s\n", bm_names[mode]);
	else
	    dprintf1("%d??\n", (int)mode);
    }
#endif
    if (mode < 0 || mode > MAX_BLEND_MODE)
	return_error(gs_error_rangecheck);
    pgs->blend_mode = mode;
    return 0;
}

gs_blend_mode_t
gs_currentblendmode(const gs_state *pgs)
{
    return pgs->blend_mode;
}

int
gs_setopacityalpha(gs_state *pgs, floatp alpha)
{
    if_debug2('v', "[v](0x%lx)opacity.alpha = %g\n", (ulong)pgs, alpha);
    pgs->opacity.alpha = (alpha < 0.0 ? 0.0 : alpha > 1.0 ? 1.0 : alpha);
    return 0;
}

float
gs_currentopacityalpha(const gs_state *pgs)
{
    return pgs->opacity.alpha;
}

int
gs_setshapealpha(gs_state *pgs, floatp alpha)
{
    if_debug2('v', "[v](0x%lx)shape.alpha = %g\n", (ulong)pgs, alpha);
    pgs->shape.alpha = (alpha < 0.0 ? 0.0 : alpha > 1.0 ? 1.0 : alpha);
    return 0;
}

float
gs_currentshapealpha(const gs_state *pgs)
{
    return pgs->shape.alpha;
}

int
gs_settextknockout(gs_state *pgs, bool knockout)
{
    if_debug2('v', "[v](0x%lx)text_knockout = %s\n", (ulong)pgs,
	      (knockout ? "true" : "false"));
    pgs->text_knockout = knockout;
    return 0;
}

bool
gs_currenttextknockout(const gs_state *pgs)
{
    return pgs->text_knockout;
}

/* ------ Transparency rendering stack ------ */

gs_transparency_state_type_t
gs_current_transparency_type(const gs_state *pgs)
{
    return (pgs->transparency_stack == 0 ? 0 :
	    pgs->transparency_stack->type);
}

/* Support for dummy implementation */
gs_private_st_ptrs1(st_transparency_state, gs_transparency_state_t,
		    "gs_transparency_state_t", transparency_state_enum_ptrs,
		    transparency_state_reloc_ptrs, saved);
#if PUSH_TS
static int
push_transparency_stack(gs_state *pgs, gs_transparency_state_type_t type,
			client_name_t cname)
{
    gs_transparency_state_t *pts =
	gs_alloc_struct(pgs->memory, gs_transparency_state_t,
			&st_transparency_state, cname);

    if (pts == 0)
	return_error(gs_error_VMerror);
    pts->saved = pgs->transparency_stack;
    pts->type = type;
    pgs->transparency_stack = pts;
    return 0;
}
#endif
static void
pop_transparency_stack(gs_state *pgs, client_name_t cname)
{
    gs_transparency_state_t *pts = pgs->transparency_stack; /* known non-0 */
    gs_transparency_state_t *saved = pts->saved;

    gs_free_object(pgs->memory, pts, cname);
    pgs->transparency_stack = saved;

}

/*
 * Push a PDF 1.4 transparency compositor onto the current device. Note that
 * if the current device already is a PDF 1.4 transparency compositor, the
 * create_compositor will update its parameters but not create a new
 * compositor device.
 */
static int
gs_state_update_pdf14trans(gs_state * pgs, gs_pdf14trans_params_t * pparams)
{
    gs_imager_state * pis = (gs_imager_state *)pgs;
    gx_device * dev = pgs->device;
    gx_device *pdf14dev = NULL;
    int code;

    /*
     * Send the PDF 1.4 create compositor action specified by the parameters.
     */
    code = send_pdf14trans(pis, dev, &pdf14dev, pparams, pgs->memory);
    /*
     * If we created a new PDF 1.4 compositor device then we need to install it
     * into the graphics state.
     */
    if (code >= 0 && pdf14dev != dev)
        gx_set_device_only(pgs, pdf14dev);

    return code;
}

void
gs_trans_group_params_init(gs_transparency_group_params_t *ptgp)
{
    ptgp->ColorSpace = 0;	/* bogus, but can't do better */
    ptgp->Isolated = false;
    ptgp->Knockout = false;
    ptgp->image_with_SMask = false;
    ptgp->mask_id = 0;
}

int
gs_begin_transparency_group(gs_state *pgs,
			    const gs_transparency_group_params_t *ptgp,
			    const gs_rect *pbbox)
{
    gs_pdf14trans_params_t params = { 0 };

#ifdef DEBUG
    if (gs_debug_c('v')) {
	static const char *const cs_names[] = {
	    GS_COLOR_SPACE_TYPE_NAMES
	};

	dlprintf5("[v](0x%lx)begin_transparency_group [%g %g %g %g]\n",
		  (ulong)pgs, pbbox->p.x, pbbox->p.y, pbbox->q.x, pbbox->q.y);
	if (ptgp->ColorSpace)
	    dprintf1("     CS = %s",
		cs_names[(int)gs_color_space_get_index(ptgp->ColorSpace)]);
	else
	    dputs("     (no CS)");
	dprintf2("  Isolated = %d  Knockout = %d\n",
		 ptgp->Isolated, ptgp->Knockout);
    }
#endif
    /*
     * Put parameters into a compositor parameter and then call the
     * create_compositor.  This will pass the data to the PDF 1.4
     * transparency device.
     */
    params.pdf14_op = PDF14_BEGIN_TRANS_GROUP;
    params.Isolated = ptgp->Isolated;
    params.Knockout = ptgp->Knockout;
    params.image_with_SMask = ptgp->image_with_SMask;
    params.opacity = pgs->opacity;
    params.shape = pgs->shape;
    params.blend_mode = pgs->blend_mode;
    /*
     * We are currently doing nothing with the colorspace.  Currently
     * the blending colorspace is based upon the processs color model
     * of the output device.
     */
    params.bbox = *pbbox;
    return gs_state_update_pdf14trans(pgs, &params);
}

int
gx_begin_transparency_group(gs_imager_state * pis, gx_device * pdev,
				const gs_pdf14trans_params_t * pparams)
{
    gs_transparency_group_params_t tgp = {0};
    gs_rect bbox;

    if (pparams->Background_components != 0 && 
	pparams->Background_components != pdev->color_info.num_components)
	return_error(gs_error_rangecheck);
    tgp.Isolated = pparams->Isolated;
    tgp.Knockout = pparams->Knockout;
    tgp.idle = pparams->idle;
    tgp.mask_id = pparams->mask_id;
    pis->opacity.alpha = pparams->opacity.alpha;
    pis->shape.alpha = pparams->shape.alpha;
    pis->blend_mode = pparams->blend_mode;
    bbox = pparams->bbox;
#ifdef DEBUG
    if (gs_debug_c('v')) {
	static const char *const cs_names[] = {
	    GS_COLOR_SPACE_TYPE_NAMES
	};

	dlprintf5("[v](0x%lx)gx_begin_transparency_group [%g %g %g %g]\n",
		  (ulong)pis, bbox.p.x, bbox.p.y, bbox.q.x, bbox.q.y);
	if (tgp.ColorSpace)
	    dprintf1("     CS = %s",
		cs_names[(int)gs_color_space_get_index(tgp.ColorSpace)]);
	else
	    dputs("     (no CS)");
	dprintf2("  Isolated = %d  Knockout = %d\n",
		 tgp.Isolated, tgp.Knockout);
    }
#endif
    if (dev_proc(pdev, begin_transparency_group) != 0)
	return (*dev_proc(pdev, begin_transparency_group)) (pdev, &tgp,
							&bbox, pis, NULL, NULL);
    else
	return 0;
}

int
gs_end_transparency_group(gs_state *pgs)
{
    gs_pdf14trans_params_t params = { 0 };

    if_debug0('v', "[v]gs_end_transparency_group\n");
    params.pdf14_op = PDF14_END_TRANS_GROUP;  /* Other parameters not used */
    return gs_state_update_pdf14trans(pgs, &params);
}

int
gx_end_transparency_group(gs_imager_state * pis, gx_device * pdev)
{
    if_debug0('v', "[v]gx_end_transparency_group\n");
    if (dev_proc(pdev, end_transparency_group) != 0)
	return (*dev_proc(pdev, end_transparency_group)) (pdev, pis, NULL);
    else
	return 0;
}

/*
 * Handler for identity mask transfer functions.
 */
static int
mask_transfer_identity(floatp in, float *out, void *proc_data)
{
    *out = (float) in;
    return 0;
}

void
gs_trans_mask_params_init(gs_transparency_mask_params_t *ptmp,
			  gs_transparency_mask_subtype_t subtype)
{
    ptmp->subtype = subtype;
    ptmp->Background_components = 0;
    ptmp->TransferFunction = mask_transfer_identity;
    ptmp->TransferFunction_data = 0;
    ptmp->replacing = false;
}

int
gs_begin_transparency_mask(gs_state * pgs,
			   const gs_transparency_mask_params_t * ptmp,
			   const gs_rect * pbbox, bool mask_is_image)
{
    gs_pdf14trans_params_t params = { 0 };
    const int l = sizeof(params.Background[0]) * ptmp->Background_components;
    int i;

    if_debug8('v', "[v](0x%lx)gs_begin_transparency_mask [%g %g %g %g]\n\
      subtype = %d  Background_components = %d  %s\n",
	      (ulong)pgs, pbbox->p.x, pbbox->p.y, pbbox->q.x, pbbox->q.y,
	      (int)ptmp->subtype, ptmp->Background_components,
	      (ptmp->TransferFunction == mask_transfer_identity ? "no TR" :
	       "has TR"));
    params.pdf14_op = PDF14_BEGIN_TRANS_MASK;
    params.bbox = *pbbox;
    params.subtype = ptmp->subtype;
    params.Background_components = ptmp->Background_components;
    memcpy(params.Background, ptmp->Background, l);
    params.GrayBackground = ptmp->GrayBackground;
    params.transfer_function = ptmp->TransferFunction_data;
    params.function_is_identity =
	    (ptmp->TransferFunction == mask_transfer_identity);
    params.mask_is_image = mask_is_image;
    params.replacing = ptmp->replacing;
    /* Sample the transfer function */
    for (i = 0; i < MASK_TRANSFER_FUNCTION_SIZE; i++) {
	float in = (float)(i * (1.0 / (MASK_TRANSFER_FUNCTION_SIZE - 1)));
	float out;

	ptmp->TransferFunction(in, &out, ptmp->TransferFunction_data);
	params.transfer_fn[i] = (byte)floor((double)(out * 255 + 0.5));
    }
    return gs_state_update_pdf14trans(pgs, &params);
}

int
gx_begin_transparency_mask(gs_imager_state * pis, gx_device * pdev,
				const gs_pdf14trans_params_t * pparams)
{
    gx_transparency_mask_params_t tmp;
    const int l = sizeof(pparams->Background[0]) * pparams->Background_components;

    tmp.subtype = pparams->subtype;
    tmp.Background_components = pparams->Background_components;
    memcpy(tmp.Background, pparams->Background, l);
    tmp.GrayBackground = pparams->GrayBackground;
    tmp.function_is_identity = pparams->function_is_identity;
    tmp.idle = pparams->idle;
    tmp.replacing = pparams->replacing;
    tmp.mask_id = pparams->mask_id;
    memcpy(tmp.transfer_fn, pparams->transfer_fn, size_of(tmp.transfer_fn));
    if_debug8('v', "[v](0x%lx)gx_begin_transparency_mask [%g %g %g %g]\n\
      subtype = %d  Background_components = %d  %s\n",
	      (ulong)pis, pparams->bbox.p.x, pparams->bbox.p.y,
	      pparams->bbox.q.x, pparams->bbox.q.y,
	      (int)tmp.subtype, tmp.Background_components,
	      (tmp.function_is_identity ? "no TR" :
	       "has TR"));
    if (dev_proc(pdev, begin_transparency_mask) != 0)
	return (*dev_proc(pdev, begin_transparency_mask))
	    		(pdev, &tmp, &(pparams->bbox), pis, NULL, NULL);
    else
	return 0;
}

int
gs_end_transparency_mask(gs_state *pgs,
			 gs_transparency_channel_selector_t csel)
{
    gs_pdf14trans_params_t params = { 0 };

    if_debug2('v', "[v](0x%lx)gs_end_transparency_mask(%d)\n", (ulong)pgs,
	      (int)csel);

    params.pdf14_op = PDF14_END_TRANS_MASK;  /* Other parameters not used */
    params.csel = csel;
    return gs_state_update_pdf14trans(pgs, &params);
}

int
gx_end_transparency_mask(gs_imager_state * pis, gx_device * pdev,
				const gs_pdf14trans_params_t * pparams)
{
    if_debug2('v', "[v](0x%lx)gx_end_transparency_mask(%d)\n", (ulong)pis,
	      (int)pparams->csel);
    if (dev_proc(pdev, end_transparency_mask) != 0)
	return (*dev_proc(pdev, end_transparency_mask)) (pdev, NULL);
    else
	return 0;
}

int
gs_discard_transparency_layer(gs_state *pgs)
{
    /****** NYI, DUMMY ******/
    gs_transparency_state_t *pts = pgs->transparency_stack;

    if_debug1('v', "[v](0x%lx)gs_discard_transparency_layer\n", (ulong)pgs);
    if (!pts)
	return_error(gs_error_rangecheck);
    pop_transparency_stack(pgs, "gs_discard_transparency_layer");
    return 0;
}

/*
 * We really only care about the number of spot colors when we have
 * a device which supports spot colors.  With the other devices we use
 * the tint transform function for DeviceN and Separation color spaces
 * and convert spot colors into process colors.
 */
static int
get_num_pdf14_spot_colors(gs_state * pgs)
{
    gx_device * dev = pgs->device;
    gs_devn_params * pclist_devn_params = dev_proc(dev, ret_devn_params)(dev);

    /*
     * Devices which support spot colors store the PageSpotColors device
     * parameter inside their devn_params structure.  (This is done by the
     * devn_put_params routine.)  The PageSpotColors device parameter is
     * set by pdf_main whenever a PDF page is being processed.  See
     * countspotcolors in lib/pdf_main.ps.
     */
    if (pclist_devn_params != NULL) {
	return pclist_devn_params->page_spot_colors;
    }
    return 0;
}

int
gs_push_pdf14trans_device(gs_state * pgs)
{
    gs_pdf14trans_params_t params = { 0 };

    params.pdf14_op = PDF14_PUSH_DEVICE;
    /*
     * We really only care about the number of spot colors when we have
     * a device which supports spot colors.  With the other devices we use
     * the tint transform function for DeviceN and Separation color spaces
     * and convert spot colors into process colors.
     */
    params.num_spot_colors = get_num_pdf14_spot_colors(pgs);
    /* Note: Other parameters not used */
    return gs_state_update_pdf14trans(pgs, &params);
}

int
gs_pop_pdf14trans_device(gs_state * pgs)
{
    gs_pdf14trans_params_t params = { 0 };

    params.pdf14_op = PDF14_POP_DEVICE;  /* Other parameters not used */
    return gs_state_update_pdf14trans(pgs, &params);
}
