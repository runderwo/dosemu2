/*
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

/*
 * swappable mouse drivers
 *
 * Author: Stas Sergeev
 */
#include <stdlib.h>
#include <string.h>
#include "emu.h"
#include "serial.h"
#include "utilities.h"
#include "mouse.h"

static struct mouse_drv_wrp *mdrv;

struct mouse_client *Mouse = NULL;

/* register mouse at the back of the linked list */
void register_mouse_client(struct mouse_client *mouse)
{
	struct mouse_client *m;

	mouse->next = NULL;
	if (Mouse == NULL)
		Mouse = mouse;
	else {
		for (m = Mouse; m->next; m = m->next);
		m->next = mouse;
	}
}

void register_mouse_driver(struct mouse_drv *mouse)
{
	struct mouse_drv_wrp *m, *ms;
	ms = malloc(sizeof(*ms));
	ms->drv = mouse;
	ms->udata = NULL;
	ms->initialized = 0;
	ms->next = NULL;
	if (mdrv == NULL)
		mdrv = ms;
	else {
		for (m = mdrv; m->next; m = m->next);
		m->next = ms;
	}
}

void mousedrv_set_udata(const char *name, void *udata)
{
	struct mouse_drv_wrp *m;
	for (m = mdrv; m->next; m = m->next) {
		if (strcmp(name, m->drv->name) == 0) {
			m->udata = udata;
			break;
		}
	}
}

static int none_init(void)
{
  return TRUE;
}

static struct mouse_client Mouse_none =  {
  "No Mouse",   /* name */
  none_init,	/* init */
  NULL,		/* close */
  NULL,		/* run */
  NULL
};

static void mouse_client_init(void)
{
  int ok;

#ifdef USE_GPM
  if (Mouse == NULL)
    load_plugin("gpm");
#endif
  register_mouse_client(&Mouse_raw);
  register_mouse_client(&Mouse_none);
  while(TRUE) {
    m_printf("MOUSE: initialising '%s' mode mouse client\n",
             Mouse->name);

    ok = Mouse->init?Mouse->init():TRUE;
    if (ok) {
      m_printf("MOUSE: Mouse init ok, '%s' mode\n", Mouse->name);
      break;
    }
    else {
      m_printf("MOUSE: Mouse init ***failed***, '%s' mode\n",
               Mouse->name);
    }
    Mouse = Mouse->next;
  }
}

void dosemu_mouse_init(void)
{
  struct mouse_drv_wrp *m;
  for (m = mdrv; m; m = m->next) {
    if (!m->drv->init || m->drv->init())
      m->initialized = 1;
  }

  mouse_client_init();
}

#define AR(...) (__VA_ARGS__, m->udata)
#define MOUSE_DO(n, DEF, ARGS) \
void mouse_##n DEF \
{ \
  struct mouse_drv_wrp *m; \
  for (m = mdrv; m; m = m->next) { \
    struct mouse_drv *d; \
    if (!m->initialized) \
      continue; \
    d = m->drv; \
    if (d->n && d->accepts(m->udata)) \
	d->n AR ARGS; \
  } \
}
MOUSE_DO(move_buttons, (int lbutton, int mbutton, int rbutton),
	(lbutton, mbutton, rbutton))
MOUSE_DO(move_relative, (int dx, int dy, int x_range, int y_range),
	(dx, dy, x_range, y_range))
MOUSE_DO(move_mickeys, (int dx, int dy), (dx, dy))
MOUSE_DO(move_absolute, (int x, int y, int x_range, int y_range),
	(x, y, x_range, y_range))
MOUSE_DO(drag_to_corner, (int x_range, int y_range), (x_range, y_range))
MOUSE_DO(sync_coords, (int x, int y, int x_range, int y_range),
	(x, y, x_range, y_range))
MOUSE_DO(enable_native_cursor, (int flag), (flag))

int mousedrv_accepts(const char *id)
{
  struct mouse_drv_wrp *m;
  for (m = mdrv; m; m = m->next) {
    struct mouse_drv *d;
    if (!m->initialized)
      continue;
    d = m->drv;
    if (strcmp(d->name, id) != 0)
      continue;
    if (d->accepts)
	return d->accepts(m->udata);
  }
  return 0;
}

#define DID(...) (__VA_ARGS__, const char *id)
#define MOUSE_ID_DO(n, DEF, ARGS) \
void mouse_##n##_id DID DEF \
{ \
  struct mouse_drv_wrp *m; \
  for (m = mdrv; m; m = m->next) { \
    struct mouse_drv *d; \
    if (!m->initialized) \
      continue; \
    d = m->drv; \
    if (strcmp(d->name, id) != 0) \
      continue; \
    /* not checking for .accpets */ \
    if (d->n) \
	d->n AR ARGS; \
  } \
}
MOUSE_ID_DO(move_buttons, (int lbutton, int mbutton, int rbutton),
	(lbutton, mbutton, rbutton))
MOUSE_ID_DO(move_mickeys, (int dx, int dy), (dx, dy))
