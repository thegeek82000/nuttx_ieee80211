/****************************************************************************
 * net/ieee80211/i22280211_node.c
 *
 * Copyright (c) 2001 Atsushi Onoe
 * Copyright (c) 2002, 2003 Sam Leffler, Errno Consulting
 * Copyright (c) 2008 Damien Bergamini
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/socket.h>

#include <string.h>
#include <wdog.h>
#include <assert.h>
#include <errno.h>
#include <debug.h>

#include <net/if.h>

#ifdef CONFIG_NET_ETHERNET
#  include <netinet/in.h>
#  include <nuttx/net/uip/uip.h>
#endif

#ifdef CONFIG_IEEE80211_BRIDGEPORT
#  include <net/if_bridge.h>
#endif

#include <nuttx/kmalloc.h>
#include <nuttx/tree.h>

#include "ieee80211/ieee80211_debug.h"
#include "ieee80211/ieee80211_ifnet.h"
#include "ieee80211/ieee80211_var.h"
#include "ieee80211/ieee80211_priv.h"

struct ieee80211_node *ieee80211_node_alloc(struct ieee80211_s *);
void ieee80211_node_free(struct ieee80211_s *, struct ieee80211_node *);
void ieee80211_node_copy(struct ieee80211_s *, struct ieee80211_node *,
                         const struct ieee80211_node *);
void ieee80211_choose_rsnparams(struct ieee80211_s *);
uint8_t ieee80211_node_getrssi(struct ieee80211_s *,
                               const struct ieee80211_node *);
void ieee80211_setup_node(struct ieee80211_s *, struct ieee80211_node *,
                          const uint8_t *);
void ieee80211_free_node(struct ieee80211_s *, struct ieee80211_node *);
struct ieee80211_node *ieee80211_alloc_node_helper(struct ieee80211_s *);
void ieee80211_node_cleanup(struct ieee80211_s *, struct ieee80211_node *);
void ieee80211_needs_auth(struct ieee80211_s *, struct ieee80211_node *);
#ifdef CONFIG_IEEE80211_AP
#  ifdef CONFIG_IEEE80211_HT
void ieee80211_node_join_ht(struct ieee80211_s *, struct ieee80211_node *);
#  endif
void ieee80211_node_join_rsn(struct ieee80211_s *, struct ieee80211_node *);
void ieee80211_node_join_11g(struct ieee80211_s *, struct ieee80211_node *);
#  ifdef CONFIG_IEEE80211_HT
void ieee80211_node_leave_ht(struct ieee80211_s *, struct ieee80211_node *);
#  endif
void ieee80211_node_leave_rsn(struct ieee80211_s *, struct ieee80211_node *);
void ieee80211_node_leave_11g(struct ieee80211_s *, struct ieee80211_node *);
void ieee80211_inact_timeout(void *);
void ieee80211_node_cache_timeout(void *);
#endif

#ifdef CONFIG_IEEE80211_AP
void ieee80211_inact_timeout(void *arg)
{
  struct ieee80211_s *ic = arg;
  struct ieee80211_node *ni, *next_ni;
  uip_lock_t flags;

  flags = uip_lock();
  for (ni = RB_MIN(ieee80211_tree, &ic->ic_tree); ni != NULL; ni = next_ni)
    {
      next_ni = RB_NEXT(ieee80211_tree, &ic->ic_tree, ni);
      if (ni->ni_refcnt > 0)
        continue;
      if (ni->ni_inact < IEEE80211_INACT_MAX)
        ni->ni_inact++;
    }
  uip_unlock(flags);

  wd_start(ic->ic_inact_timeout, SEC2TICK(IEEE80211_INACT_WAIT),
           ieee80211_inact_timeout, ic);
}

void ieee80211_node_cache_timeout(void *arg)
{
  struct ieee80211_s *ic = arg;

  ieee80211_clean_nodes(ic, 1);
  wd_start(ic->ic_node_cache_timeout, SEC2TICK(IEEE80211_CACHE_WAIT),
           ieee80211_node_cache_timeout, ic);
}
#endif

void ieee80211_node_attach(struct ieee80211_s *ic)
{
#ifdef CONFIG_IEEE80211_AP
  int size;
#endif

  RB_INIT(&ic->ic_tree);
  ic->ic_node_alloc = ieee80211_node_alloc;
  ic->ic_node_free = ieee80211_node_free;
  ic->ic_node_copy = ieee80211_node_copy;
  ic->ic_node_getrssi = ieee80211_node_getrssi;
  ic->ic_scangen = 1;
  ic->ic_max_nnodes = ieee80211_cache_size;

  if (ic->ic_max_aid == 0)
    ic->ic_max_aid = IEEE80211_AID_DEF;
  else if (ic->ic_max_aid > IEEE80211_AID_MAX)
    ic->ic_max_aid = IEEE80211_AID_MAX;
#ifdef CONFIG_IEEE80211_AP
  size = howmany(ic->ic_max_aid, 32) * sizeof(uint32_t);
  ic->ic_aid_bitmap = kmalloc(size);
  if (ic->ic_aid_bitmap == NULL)
    {
      /* XXX no way to recover */

      nvdbg("No memory for AID bitmap!\n");
      ic->ic_max_aid = 0;
    }

  if (ic->ic_caps & (IEEE80211_C_HOSTAP | IEEE80211_C_IBSS))
    {
      ic->ic_tim_len = howmany(ic->ic_max_aid, 8);
      ic->ic_tim_bitmap = kmalloc(ic->ic_tim_len);
      if (ic->ic_tim_bitmap == NULL)
        {
          nvdbg("No memory for TIM bitmap!\n");
          ic->ic_tim_len = 0;
        }
      else
        {
          ic->ic_set_tim = ieee80211_set_tim;
        }

      ic->ic_rsn_timeout = wd_create();
      ic->ic_inact_timeout = wd_create();
      ic->ic_node_cache_timeout = wd_create();
    }
#endif
}

struct ieee80211_node *ieee80211_alloc_node_helper(struct ieee80211_s *ic)
{
  struct ieee80211_node *ni;
  if (ic->ic_nnodes >= ic->ic_max_nnodes)
    ieee80211_clean_nodes(ic, 0);
  if (ic->ic_nnodes >= ic->ic_max_nnodes)
    return NULL;
  ni = (*ic->ic_node_alloc) (ic);
  return ni;
}

void ieee80211_node_lateattach(struct ieee80211_s *ic)
{
  struct ieee80211_node *ni;

  ni = ieee80211_alloc_node_helper(ic);
  DEBUGASSERT(ni != NULL);

  ni->ni_chan = IEEE80211_CHAN_ANYC;
  ic->ic_bss = ieee80211_ref_node(ni);
  ic->ic_txpower = IEEE80211_TXPOWER_MAX;
}

void ieee80211_node_detach(struct ieee80211_s *ic)
{
  if (ic->ic_bss != NULL)
    {
      (*ic->ic_node_free) (ic, ic->ic_bss);
      ic->ic_bss = NULL;
    }

  ieee80211_free_allnodes(ic);

#ifdef CONFIG_IEEE80211_AP
  if (ic->ic_aid_bitmap != NULL)
    {
      kfree(ic->ic_aid_bitmap);
    }

  if (ic->ic_tim_bitmap != NULL)
    {
      kfree(ic->ic_tim_bitmap);
    }

  wd_cancel(ic->ic_inact_timeout);
  wd_cancel(ic->ic_node_cache_timeout);
#endif
  wd_cancel(ic->ic_rsn_timeout);
}

/* AP scanning support */

/* Initialize the active channel set based on the set
 * of available channels and the current PHY mode.
 */

void ieee80211_reset_scan(FAR struct ieee80211_s *ic)
{
  memcpy(ic->ic_chan_scan, ic->ic_chan_active, sizeof(ic->ic_chan_active));

  /* NB: hack, setup so next_scan starts with the first channel */

  if (ic->ic_bss != NULL && ic->ic_bss->ni_chan == IEEE80211_CHAN_ANYC)
    {
      ic->ic_bss->ni_chan = &ic->ic_channels[IEEE80211_CHAN_MAX];
    }
}

/* Begin an active scan */

void ieee80211_begin_scan(struct ieee80211_s *ic)
{
  if (ic->ic_scan_lock & IEEE80211_SCAN_LOCKED)
    {
      return;
    }

  ic->ic_scan_lock |= IEEE80211_SCAN_LOCKED;

  /* In all but hostap mode scanning starts off in an active mode before
   * switching to passive. */

#ifdef CONFIG_IEEE80211_AP
  if (ic->ic_opmode != IEEE80211_M_HOSTAP)
#endif
    {
      ic->ic_flags |= IEEE80211_F_ASCAN;
    }

  nvdbg("%s: begin %s scan\n",
        ic->ic_ifname,
        (ic->ic_flags & IEEE80211_F_ASCAN) ? "active" : "passive");

  /* Flush any previously seen AP's. Note that the latter assumes we don't act
   * as both an AP and a station, otherwise we'll potentially flush state of
   * stations associated with us.
   */

  ieee80211_free_allnodes(ic);

  /* Reset the current mode. Setting the current mode will also reset scan
   * state.
   */

  ieee80211_setmode(ic, ic->ic_curmode);
  ic->ic_scan_count = 0;

  /* Scan the next channel. */

  ieee80211_next_scan(ic);
}

/* Switch to the next channel marked for scanning */

void ieee80211_next_scan(struct ieee80211_s *ic)
{
  struct ieee80211_channel *chan;
  int ibss;
  int ndx;
  int bit;

  chan = ic->ic_bss->ni_chan;
  for (;;)
    {
      if (++chan > &ic->ic_channels[IEEE80211_CHAN_MAX])
        {
          chan = &ic->ic_channels[0];
        }

      ibss = ieee80211_chan2ieee(ic, chan);
      ndx = (ibss >> 3);
      bit = (ibss & 7);

      if ((ic->ic_chan_scan[ndx] & (1 << bit)) != 0)
        {
          /* Ignore channels marked passive-only during an active scan */

          if ((ic->ic_flags & IEEE80211_F_ASCAN) == 0 ||
              (chan->ic_flags & IEEE80211_CHAN_PASSIVE) == 0)
            {
              break;
            }
        }

      if (chan == ic->ic_bss->ni_chan)
        {
          ieee80211_end_scan(ic);
          return;
        }
    }

  ibss = ieee80211_chan2ieee(ic, chan);
  ndx = (ibss >> 3);
  bit = (ibss & 7);
  ic->ic_chan_scan[ndx] &= ~(1 << bit);

  nvdbg("chan %d->%d\n",
        ieee80211_chan2ieee(ic, ic->ic_bss->ni_chan),
        ieee80211_chan2ieee(ic, chan));

  ic->ic_bss->ni_chan = chan;
  ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
}

#ifdef CONFIG_IEEE80211_AP
void ieee80211_create_ibss(struct ieee80211_s *ic,
                           struct ieee80211_channel *chan)
{
  struct ieee80211_node *ni;

  ni = ic->ic_bss;
  nvdbg("%s: creating ibss\n", ic->ic_ifname);

  ic->ic_flags |= IEEE80211_F_SIBSS;
  ni->ni_chan = chan;
  ni->ni_rates = ic->ic_sup_rates[ieee80211_chan2mode(ic, ni->ni_chan)];
  ni->ni_txrate = 0;
  IEEE80211_ADDR_COPY(ni->ni_macaddr, ic->ic_myaddr);
  IEEE80211_ADDR_COPY(ni->ni_bssid, ic->ic_myaddr);
  if (ic->ic_opmode == IEEE80211_M_IBSS)
    {
      if ((ic->ic_flags & IEEE80211_F_DESBSSID) != 0)
        IEEE80211_ADDR_COPY(ni->ni_bssid, ic->ic_des_bssid);
      else
        ni->ni_bssid[0] |= 0x02;        /* local bit for IBSS */
    }
  ni->ni_esslen = ic->ic_des_esslen;
  memcpy(ni->ni_essid, ic->ic_des_essid, ni->ni_esslen);
  ni->ni_rssi = 0;
  ni->ni_rstamp = 0;
  memset(ni->ni_tstamp, 0, sizeof(ni->ni_tstamp));
  ni->ni_intval = ic->ic_lintval;
  ni->ni_capinfo = IEEE80211_CAPINFO_IBSS;
  if (ic->ic_flags & IEEE80211_F_WEPON)
    ni->ni_capinfo |= IEEE80211_CAPINFO_PRIVACY;
  if (ic->ic_flags & IEEE80211_F_RSNON)
    {
      struct ieee80211_key *k;

      /* initialize 256-bit global key counter to a random value */

      arc4random_buf(ic->ic_globalcnt, EAPOL_KEY_NONCE_LEN);

      ni->ni_rsnprotos = ic->ic_rsnprotos;
      ni->ni_rsnakms = ic->ic_rsnakms;
      ni->ni_rsnciphers = ic->ic_rsnciphers;
      ni->ni_rsngroupcipher = ic->ic_rsngroupcipher;
      ni->ni_rsngroupmgmtcipher = ic->ic_rsngroupmgmtcipher;
      ni->ni_rsncaps = 0;
      if (ic->ic_caps & IEEE80211_C_MFP)
        {
          ni->ni_rsncaps |= IEEE80211_RSNCAP_MFPC;
          if (ic->ic_flags & IEEE80211_F_MFPR)
            ni->ni_rsncaps |= IEEE80211_RSNCAP_MFPR;
        }

      ic->ic_def_txkey = 1;
      k = &ic->ic_nw_keys[ic->ic_def_txkey];
      memset(k, 0, sizeof(*k));
      k->k_id = ic->ic_def_txkey;
      k->k_cipher = ni->ni_rsngroupcipher;
      k->k_flags = IEEE80211_KEY_GROUP | IEEE80211_KEY_TX;
      k->k_len = ieee80211_cipher_keylen(k->k_cipher);
      arc4random_buf(k->k_key, k->k_len);
      (*ic->ic_set_key) (ic, ni, k);    /* XXX */

      if (ic->ic_caps & IEEE80211_C_MFP)
        {
          ic->ic_igtk_kid = 4;
          k = &ic->ic_nw_keys[ic->ic_igtk_kid];
          memset(k, 0, sizeof(*k));
          k->k_id = ic->ic_igtk_kid;
          k->k_cipher = ni->ni_rsngroupmgmtcipher;
          k->k_flags = IEEE80211_KEY_IGTK | IEEE80211_KEY_TX;
          k->k_len = 16;
          arc4random_buf(k->k_key, k->k_len);
          (*ic->ic_set_key) (ic, ni, k);        /* XXX */
        }

      /* In HostAP mode, multicast traffic is sent using ic_bss as the Tx node, 
       * so mark our node as valid so we can send multicast frames using the
       * group key we've just configured.
       */

      ni->ni_port_valid = 1;
      ni->ni_flags |= IEEE80211_NODE_TXPROT;

      /* schedule a GTK/IGTK rekeying after 3600s */

      wd_start(ic->ic_rsn_timeout, SEC2TICK(3600), ieee80211_gtk_rekey_timeout,
               1, ic);
    }
  wd_start(ic->ic_inact_timeout, SEC2TICK(IEEE80211_INACT_WAIT),
           ieee80211_inact_timeout, ic);
  wd_start(ic->ic_node_cache_timeout, SEC2TICK(IEEE80211_CACHE_WAIT),
           ieee80211_node_cache_timeout, ic);
  ieee80211_new_state(ic, IEEE80211_S_RUN, -1);
}
#endif /* CONFIG_IEEE80211_AP */

int ieee80211_match_bss(struct ieee80211_s *ic, struct ieee80211_node *ni)
{
  uint8_t rate;
  int fail;
  int ibss;
  int ndx;
  int bit;

  fail = 0;
  ibss = ieee80211_chan2ieee(ic, ni->ni_chan);
  ndx = (ibss >> 3);
  bit = (ibss & 7);

  if ((ic->ic_chan_active[ndx] & (1 << bit)) == 0)
    {
      fail |= 0x01;
    }

  if (ic->ic_des_chan != IEEE80211_CHAN_ANYC && ni->ni_chan != ic->ic_des_chan)
    fail |= 0x01;
#ifdef CONFIG_IEEE80211_AP
  if (ic->ic_opmode == IEEE80211_M_IBSS)
    {
      if ((ni->ni_capinfo & IEEE80211_CAPINFO_IBSS) == 0)
        fail |= 0x02;
    }
  else
#endif
    {
      if ((ni->ni_capinfo & IEEE80211_CAPINFO_ESS) == 0)
        fail |= 0x02;
    }
  if (ic->ic_flags & (IEEE80211_F_WEPON | IEEE80211_F_RSNON))
    {
      if ((ni->ni_capinfo & IEEE80211_CAPINFO_PRIVACY) == 0)
        fail |= 0x04;
    }
  else
    {
      if (ni->ni_capinfo & IEEE80211_CAPINFO_PRIVACY)
        fail |= 0x04;
    }

  rate = ieee80211_fix_rate(ic, ni, IEEE80211_F_DONEGO);
  if (rate & IEEE80211_RATE_BASIC)
    fail |= 0x08;
  if (ic->ic_des_esslen != 0 &&
      (ni->ni_esslen != ic->ic_des_esslen ||
       memcmp(ni->ni_essid, ic->ic_des_essid, ic->ic_des_esslen) != 0))
    fail |= 0x10;
  if ((ic->ic_flags & IEEE80211_F_DESBSSID) &&
      !IEEE80211_ADDR_EQ(ic->ic_des_bssid, ni->ni_bssid))
    fail |= 0x20;

  if (ic->ic_flags & IEEE80211_F_RSNON)
    {
      /* If at least one RSN IE field from the AP's RSN IE fails to overlap
       * with any value the STA supports, the STA shall decline to associate
       * with that AP.
       */

      if ((ni->ni_rsnprotos & ic->ic_rsnprotos) == 0)
        fail |= 0x40;
      if ((ni->ni_rsnakms & ic->ic_rsnakms) == 0)
        fail |= 0x40;
      if ((ni->ni_rsnakms & ic->ic_rsnakms &
           ~(IEEE80211_AKM_PSK | IEEE80211_AKM_SHA256_PSK)) == 0)
        {
          /* AP only supports PSK AKMPs */

          if (!(ic->ic_flags & IEEE80211_F_PSK))
            fail |= 0x40;
        }

      if (ni->ni_rsngroupcipher != IEEE80211_CIPHER_WEP40 &&
          ni->ni_rsngroupcipher != IEEE80211_CIPHER_TKIP &&
          ni->ni_rsngroupcipher != IEEE80211_CIPHER_CCMP &&
          ni->ni_rsngroupcipher != IEEE80211_CIPHER_WEP104)
        fail |= 0x40;
      if ((ni->ni_rsnciphers & ic->ic_rsnciphers) == 0)
        fail |= 0x40;

      /* we only support BIP as the IGTK cipher */

      if ((ni->ni_rsncaps & IEEE80211_RSNCAP_MFPC) &&
          ni->ni_rsngroupmgmtcipher != IEEE80211_CIPHER_BIP)
        fail |= 0x40;

      /* we do not support MFP but AP requires it */

      if (!(ic->ic_caps & IEEE80211_C_MFP) &&
          (ni->ni_rsncaps & IEEE80211_RSNCAP_MFPR))
        fail |= 0x40;

      /* we require MFP but AP does not support it */

      if ((ic->ic_caps & IEEE80211_C_MFP) &&
          (ic->ic_flags & IEEE80211_F_MFPR) &&
          !(ni->ni_rsncaps & IEEE80211_RSNCAP_MFPC))
        fail |= 0x40;
    }

#if defined(CONFIG_DEBUG_NET) && defined(CONFIG_DEBUG_VERBOSE)
  nvdbg(" %c %s", fail ? '-' : '+', ieee80211_addr2str(ni->ni_macaddr));
  nvdbg(" %s%c", ieee80211_addr2str(ni->ni_bssid), fail & 0x20 ? '!' : ' ');
  nvdbg(" %3d%c",
        ieee80211_chan2ieee(ic, ni->ni_chan), fail & 0x01 ? '!' : ' ');
  nvdbg(" %+4d", ni->ni_rssi);
  nvdbg(" %2dM%c", (rate & IEEE80211_RATE_VAL) / 2, fail & 0x08 ? '!' : ' ');
  nvdbg(" %4s%c",
        (ni->ni_capinfo & IEEE80211_CAPINFO_ESS) ? "ess" :
        (ni->ni_capinfo & IEEE80211_CAPINFO_IBSS) ? "ibss" : "????",
        fail & 0x02 ? '!' : ' ');
  nvdbg(" %7s%c ",
        (ni->ni_capinfo & IEEE80211_CAPINFO_PRIVACY) ? "privacy" : "no",
        fail & 0x04 ? '!' : ' ');
  nvdbg(" %3s%c ",
        (ic->ic_flags & IEEE80211_F_RSNON) ? "rsn" : "no",
        fail & 0x40 ? '!' : ' ');
  ieee80211_print_essid(ni->ni_essid, ni->ni_esslen);
  nvdbg("%s\n", fail & 0x10 ? "!" : "");
#endif

  return fail;
}

/* Complete a scan of potential channels */

void ieee80211_end_scan(struct ieee80211_s *ic)
{
  struct ieee80211_node *ni;
  struct ieee80211_node *nextbs;
  struct ieee80211_node *selbs;
#ifdef CONFIG_IEEE80211_AP
  int ndx;
  int bit;
#endif

  nvdbg("%s: end %s scan\n",
        ic->ic_ifname,
        (ic->ic_flags & IEEE80211_F_ASCAN) ? "active" : "passive");

  if (ic->ic_scan_count)
    ic->ic_flags &= ~IEEE80211_F_ASCAN;

  ni = RB_MIN(ieee80211_tree, &ic->ic_tree);

#ifdef CONFIG_IEEE80211_AP
  if (ic->ic_opmode == IEEE80211_M_HOSTAP)
    {
      /* XXX off stack? */

      uint8_t occupied[howmany(IEEE80211_CHAN_MAX, 8)];
      int fail;
      int i;

      /* The passive scan to look for existing AP's completed, select a channel 
       * to camp on.  Identify the channels that already have one or more AP's
       * and try to locate an unoccupied one.  If that fails, pick a random
       * channel from the active set.
       */

      memset(occupied, 0, sizeof(occupied));
      RB_FOREACH(ni, ieee80211_tree, &ic->ic_tree)
      {
        int chan = ieee80211_chan2ieee(ic, ni->ni_chan) ndx = (chan >> 3);
        bit = (chan & 7);

        occupied[ndx] |= (1 << bit);
      }

      for (i = 0; i < IEEE80211_CHAN_MAX; i++)
        {
          ndx = (chan >> 3);
          bit = (chan & 7);

          if ((ic->ic_chan_active[ndx] & (1 << bit)) != 0 &&
              (ic->occupied[ndx] & (1 << bit)) == 0)
            {
              break;
            }
        }

      if (i == IEEE80211_CHAN_MAX)
        {
          fail = arc4random() & 3;      /* random 0-3 */
          for (i = 0; i < IEEE80211_CHAN_MAX; i++)
            {
              ndx = (i >> 3);
              bit = (i & 7);

              if ((ic->ic_chan_active[ndx] & (1 << bit)) != 0 && fail-- == 0)
                {
                  break;
                }
            }
        }

      ieee80211_create_ibss(ic, &ic->ic_channels[i]);
      goto wakeup;
    }
#endif

  if (ni == NULL)
    {
      ndbg("ERROR: no scan candidate\n");
    notfound:

#ifdef CONFIG_IEEE80211_AP
      if (ic->ic_opmode == IEEE80211_M_IBSS &&
          (ic->ic_flags & IEEE80211_F_IBSSON) && ic->ic_des_esslen != 0)
        {
          ieee80211_create_ibss(ic, ic->ic_ibss_chan);
          goto wakeup;
        }
#endif
      /* Scan the next mode if nothing has been found. This is necessary if the 
       * device supports different incompatible modes in the same channel
       * range, like like 11b and "pure" 11G mode. This will loop forever
       * except for user-initiated scans.
       */

      if (ieee80211_next_mode(ic) == IEEE80211_MODE_AUTO)
        {
          if (ic->ic_scan_lock & IEEE80211_SCAN_REQUEST &&
              ic->ic_scan_lock & IEEE80211_SCAN_RESUME)
            {
              ic->ic_scan_lock = IEEE80211_SCAN_LOCKED;

              /* Return from an user-initiated scan */

              wakeup(&ic->ic_scan_lock);
            }
          else if (ic->ic_scan_lock & IEEE80211_SCAN_REQUEST)
            goto wakeup;
          ic->ic_scan_count++;
        }

      /* Reset the list of channels to scan and start again. */

      ieee80211_next_scan(ic);
      return;
    }
  selbs = NULL;

  for (; ni != NULL; ni = nextbs)
    {
      nextbs = RB_NEXT(ieee80211_tree, &ic->ic_tree, ni);
      if (ni->ni_fails)
        {
          /* The configuration of the access points may change during my scan.
           * So delete the entry for the AP and retry to associate if there is
           * another beacon.
           */

          if (ni->ni_fails++ > 2)
            {
              ieee80211_free_node(ic, ni);
            }

          continue;
        }

      if (ieee80211_match_bss(ic, ni) == 0)
        {
          if (selbs == NULL)
            {
              selbs = ni;
            }
          else if (ni->ni_rssi > selbs->ni_rssi)
            {
              selbs = ni;
            }
        }
    }

  if (selbs == NULL)
    goto notfound;
  (*ic->ic_node_copy) (ic, ic->ic_bss, selbs);
  ni = ic->ic_bss;

  /* Set the erp state (mostly the slot time) to deal with the auto-select
   * case; this should be redundant if the mode is locked.
   */

  ic->ic_curmode = ieee80211_chan2mode(ic, ni->ni_chan);
  ieee80211_reset_erp(ic);

  if (ic->ic_flags & IEEE80211_F_RSNON)
    ieee80211_choose_rsnparams(ic);
  else if (ic->ic_flags & IEEE80211_F_WEPON)
    ni->ni_rsncipher = IEEE80211_CIPHER_USEGROUP;

  ieee80211_node_newstate(selbs, IEEE80211_STA_BSS);
#ifdef CONFIG_IEEE80211_AP
  if (ic->ic_opmode == IEEE80211_M_IBSS)
    {
      ieee80211_fix_rate(ic, ni, IEEE80211_F_DOFRATE |
                         IEEE80211_F_DONEGO | IEEE80211_F_DODEL);
      if (ni->ni_rates.rs_nrates == 0)
        goto notfound;
      ieee80211_new_state(ic, IEEE80211_S_RUN, -1);
    }
  else
#endif
    ieee80211_new_state(ic, IEEE80211_S_AUTH, -1);

wakeup:
  if (ic->ic_scan_lock & IEEE80211_SCAN_REQUEST)
    {
      /* Return from an user-initiated scan */

      wakeup(&ic->ic_scan_lock);
    }

  ic->ic_scan_lock = IEEE80211_SCAN_UNLOCKED;
}

/* Autoselect the best RSN parameters (protocol, AKMP, pairwise cipher...)
 * that are supported by both peers (STA mode only).
 */

void ieee80211_choose_rsnparams(struct ieee80211_s *ic)
{
  struct ieee80211_node *ni = ic->ic_bss;
  struct ieee80211_pmk *pmk;

  /* filter out unsupported protocol versions */

  ni->ni_rsnprotos &= ic->ic_rsnprotos;

  /* prefer RSN (aka WPA2) over WPA */

  if (ni->ni_rsnprotos & IEEE80211_PROTO_RSN)
    ni->ni_rsnprotos = IEEE80211_PROTO_RSN;
  else
    ni->ni_rsnprotos = IEEE80211_PROTO_WPA;

  /* filter out unsupported AKMPs */

  ni->ni_rsnakms &= ic->ic_rsnakms;

  /* prefer SHA-256 based AKMPs */

  if ((ic->ic_flags & IEEE80211_F_PSK) && (ni->ni_rsnakms &
                                           (IEEE80211_AKM_PSK |
                                            IEEE80211_AKM_SHA256_PSK)))
    {
      /* AP supports PSK AKMP and a PSK is configured */

      if (ni->ni_rsnakms & IEEE80211_AKM_SHA256_PSK)
        ni->ni_rsnakms = IEEE80211_AKM_SHA256_PSK;
      else
        ni->ni_rsnakms = IEEE80211_AKM_PSK;
    }
  else
    {
      if (ni->ni_rsnakms & IEEE80211_AKM_SHA256_8021X)
        ni->ni_rsnakms = IEEE80211_AKM_SHA256_8021X;
      else
        ni->ni_rsnakms = IEEE80211_AKM_8021X;

      /* check if we have a cached PMK for this AP */

      if (ni->ni_rsnprotos == IEEE80211_PROTO_RSN &&
          (pmk = ieee80211_pmksa_find(ic, ni, NULL)) != NULL)
        {
          memcpy(ni->ni_pmkid, pmk->pmk_pmkid, IEEE80211_PMKID_LEN);
          ni->ni_flags |= IEEE80211_NODE_PMKID;
        }
    }

  /* filter out unsupported pairwise ciphers */

  ni->ni_rsnciphers &= ic->ic_rsnciphers;

  /* prefer CCMP over TKIP */

  if (ni->ni_rsnciphers & IEEE80211_CIPHER_CCMP)
    ni->ni_rsnciphers = IEEE80211_CIPHER_CCMP;
  else
    ni->ni_rsnciphers = IEEE80211_CIPHER_TKIP;
  ni->ni_rsncipher = ni->ni_rsnciphers;

  /* use MFP if we both support it */

  if ((ic->ic_caps & IEEE80211_C_MFP) &&
      (ni->ni_rsncaps & IEEE80211_RSNCAP_MFPC))
    ni->ni_flags |= IEEE80211_NODE_MFP;
}

int ieee80211_get_rate(struct ieee80211_s *ic)
{
  uint8_t(*rates)[IEEE80211_RATE_MAXSIZE];
  int rate;

  rates = &ic->ic_bss->ni_rates.rs_rates;

  if (ic->ic_fixed_rate != -1)
    rate = (*rates)[ic->ic_fixed_rate];
  else if (ic->ic_state == IEEE80211_S_RUN)
    rate = (*rates)[ic->ic_bss->ni_txrate];
  else
    rate = 0;

  return rate & IEEE80211_RATE_VAL;
}

struct ieee80211_node *ieee80211_node_alloc(struct ieee80211_s *ic)
{
  return kmalloc(sizeof(struct ieee80211_node));
}

void ieee80211_node_cleanup(struct ieee80211_s *ic, struct ieee80211_node *ni)
{
  if (ni->ni_rsnie != NULL)
    {
      kfree(ni->ni_rsnie);
      ni->ni_rsnie = NULL;
    }
}

void ieee80211_node_free(struct ieee80211_s *ic, struct ieee80211_node *ni)
{
  ieee80211_node_cleanup(ic, ni);
  kfree(ni);
}

void ieee80211_node_copy(struct ieee80211_s *ic,
                         struct ieee80211_node *dst,
                         const struct ieee80211_node *src)
{
  ieee80211_node_cleanup(ic, dst);
  *dst = *src;
  dst->ni_rsnie = NULL;
  if (src->ni_rsnie != NULL)
    ieee80211_save_ie(src->ni_rsnie, &dst->ni_rsnie);
}

uint8_t ieee80211_node_getrssi(struct ieee80211_s *ic,
                               const struct ieee80211_node *ni)
{
  return ni->ni_rssi;
}

void ieee80211_setup_node(struct ieee80211_s *ic,
                          struct ieee80211_node *ni, const uint8_t * macaddr)
{
  uip_lock_t flags;

  nvdbg("%s\n", ieee80211_addr2str((uint8_t *) macaddr));
  IEEE80211_ADDR_COPY(ni->ni_macaddr, macaddr);
  ieee80211_node_newstate(ni, IEEE80211_STA_CACHE);

  ni->ni_ic = ic;               /* back-pointer */
#ifdef CONFIG_IEEE80211_AP
  ni->ni_eapol_to = wd_create();
  ni->ni_sa_query_to = wd_create();
#endif
  flags = uip_lock();
  RB_INSERT(ieee80211_tree, &ic->ic_tree, ni);
  ic->ic_nnodes++;
  uip_unlock(flags);
}

struct ieee80211_node *ieee80211_alloc_node(struct ieee80211_s *ic,
                                            const uint8_t * macaddr)
{
  struct ieee80211_node *ni = ieee80211_alloc_node_helper(ic);
  if (ni != NULL)
    {
      ieee80211_setup_node(ic, ni, macaddr);
    }

  return ni;
}

struct ieee80211_node *ieee80211_dup_bss(struct ieee80211_s *ic,
                                         const uint8_t * macaddr)
{
  struct ieee80211_node *ni = ieee80211_alloc_node_helper(ic);
  if (ni != NULL)
    {
      ieee80211_setup_node(ic, ni, macaddr);

      /* Inherit from ic_bss */

      IEEE80211_ADDR_COPY(ni->ni_bssid, ic->ic_bss->ni_bssid);
      ni->ni_chan = ic->ic_bss->ni_chan;
    }

  return ni;
}

struct ieee80211_node *ieee80211_find_node(struct ieee80211_s *ic,
                                           const uint8_t * macaddr)
{
  struct ieee80211_node *ni;
  int cmp;

  /* similar to RB_FIND except we compare keys, not nodes */

  ni = RB_ROOT(&ic->ic_tree);
  while (ni != NULL)
    {
      cmp = memcmp(macaddr, ni->ni_macaddr, IEEE80211_ADDR_LEN);
      if (cmp < 0)
        ni = RB_LEFT(ni, ni_node);
      else if (cmp > 0)
        ni = RB_RIGHT(ni, ni_node);
      else
        break;
    }
  return ni;
}

/* Return a reference to the appropriate node for sending
 * a data frame.  This handles node discovery in adhoc networks.
 *
 * Drivers will call this, so increase the reference count before
 * returning the node.
 */

struct ieee80211_node *ieee80211_find_txnode(struct ieee80211_s *ic,
                                             const uint8_t * macaddr)
{
#ifdef CONFIG_IEEE80211_AP
  struct ieee80211_node *ni;
  uip_lock_t flags;
#endif

  /* The destination address should be in the node table unless we are
   * operating in station mode or this is a multicast/broadcast frame.
   */

  if (ic->ic_opmode == IEEE80211_M_STA || IEEE80211_IS_MULTICAST(macaddr))
    return ieee80211_ref_node(ic->ic_bss);

#ifdef CONFIG_IEEE80211_AP
  flags = uip_lock();
  ni = ieee80211_find_node(ic, macaddr);
  uip_unlock(flags);
  if (ni == NULL)
    {
      if (ic->ic_opmode != IEEE80211_M_IBSS &&
          ic->ic_opmode != IEEE80211_M_AHDEMO)
        return NULL;

      /* Fake up a node; this handles node discovery in adhoc mode.  Note that
       * for the driver's benefit we we treat this like an association so the
       * driver has an opportunity to setup its private state. XXX need better 
       * way to handle this; issue probe request so we can deduce rate set,
       * etc.
       */

      if ((ni = ieee80211_dup_bss(ic, macaddr)) == NULL)
        return NULL;

      /* XXX no rate negotiation; just dup */

      ni->ni_rates = ic->ic_bss->ni_rates;
      ni->ni_txrate = 0;
      if (ic->ic_newassoc)
        (*ic->ic_newassoc) (ic, ni, 1);
    }
  return ieee80211_ref_node(ni);
#else
  return NULL;                  /* can't get there */
#endif /* CONFIG_IEEE80211_AP */
}

/* It is usually desirable to process a Rx packet using its sender's
 * node-record instead of the BSS record.
 *
 * - AP mode: keep a node-record for every authenticated/associated
 *   station *in the BSS*. For future use, we also track neighboring
 *   APs, since they might belong to the same ESS.  APs in the same
 *   ESS may bridge packets to each other, forming a Wireless
 *   Distribution System (WDS).
 *
 * - IBSS mode: keep a node-record for every station *in the BSS*.
 *   Also track neighboring stations by their beacons/probe responses.
 *
 * - monitor mode: keep a node-record for every sender, regardless
 *   of BSS.
 *
 * - STA mode: the only available node-record is the BSS record,
 *   ic->ic_bss.
 *
 * Of all the 802.11 Control packets, only the node-records for
 * RTS packets node-record can be looked up.
 *
 * Return non-zero if the packet's node-record is kept, zero
 * otherwise.
 */

static __inline int ieee80211_needs_rxnode(struct ieee80211_s *ic,
                                           const struct ieee80211_frame *wh,
                                           const uint8_t ** bssid)
{
  int monitor, rc = 0;

  monitor = (ic->ic_opmode == IEEE80211_M_MONITOR);

  *bssid = NULL;

  switch (wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK)
    {
    case IEEE80211_FC0_TYPE_CTL:
      if (!monitor)
        break;
      return (wh->i_fc[0] & IEEE80211_FC0_SUBTYPE_MASK) ==
        IEEE80211_FC0_SUBTYPE_RTS;
    case IEEE80211_FC0_TYPE_MGT:
      *bssid = wh->i_addr3;
      switch (wh->i_fc[0] & IEEE80211_FC0_SUBTYPE_MASK)
        {
        case IEEE80211_FC0_SUBTYPE_BEACON:
        case IEEE80211_FC0_SUBTYPE_PROBE_RESP:
          break;
        default:
#ifdef CONFIG_IEEE80211_AP
          if (ic->ic_opmode == IEEE80211_M_STA)
            break;
          rc = IEEE80211_ADDR_EQ(*bssid, ic->ic_bss->ni_bssid) ||
            IEEE80211_ADDR_EQ(*bssid, etherbroadcastaddr);
#endif
          break;
        }
      break;
    case IEEE80211_FC0_TYPE_DATA:
      switch (wh->i_fc[1] & IEEE80211_FC1_DIR_MASK)
        {
        case IEEE80211_FC1_DIR_NODS:
          *bssid = wh->i_addr3;
#ifdef CONFIG_IEEE80211_AP
          if (ic->ic_opmode == IEEE80211_M_IBSS ||
              ic->ic_opmode == IEEE80211_M_AHDEMO)
            rc = IEEE80211_ADDR_EQ(*bssid, ic->ic_bss->ni_bssid);
#endif
          break;
        case IEEE80211_FC1_DIR_TODS:
          *bssid = wh->i_addr1;
#ifdef CONFIG_IEEE80211_AP
          if (ic->ic_opmode == IEEE80211_M_HOSTAP)
            rc = IEEE80211_ADDR_EQ(*bssid, ic->ic_bss->ni_bssid);
#endif
          break;
        case IEEE80211_FC1_DIR_FROMDS:
        case IEEE80211_FC1_DIR_DSTODS:
          *bssid = wh->i_addr2;
#ifdef CONFIG_IEEE80211_AP
          rc = (ic->ic_opmode == IEEE80211_M_HOSTAP);
#endif
          break;
        }
      break;
    }
  return monitor || rc;
}

/* Drivers call this, so increase the reference count before returning
 * the node.
 */

struct ieee80211_node *ieee80211_find_rxnode(struct ieee80211_s *ic,
                                             const struct ieee80211_frame *wh)
{
  static const uint8_t zero[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
  struct ieee80211_node *ni;
  const uint8_t *bssid;
  uip_lock_t flags;

  if (!ieee80211_needs_rxnode(ic, wh, &bssid))
    return ieee80211_ref_node(ic->ic_bss);

  flags = uip_lock();
  ni = ieee80211_find_node(ic, wh->i_addr2);
  uip_unlock(flags);

  if (ni != NULL)
    return ieee80211_ref_node(ni);
#ifdef CONFIG_IEEE80211_AP
  if (ic->ic_opmode == IEEE80211_M_HOSTAP)
    return ieee80211_ref_node(ic->ic_bss);
#endif

  /* XXX see remarks in ieee80211_find_txnode */
  /* XXX no rate negotiation; just dup */

  if ((ni = ieee80211_dup_bss(ic, wh->i_addr2)) == NULL)
    return ieee80211_ref_node(ic->ic_bss);

  IEEE80211_ADDR_COPY(ni->ni_bssid, (bssid != NULL) ? bssid : zero);

  ni->ni_rates = ic->ic_bss->ni_rates;
  ni->ni_txrate = 0;
  if (ic->ic_newassoc)
    (*ic->ic_newassoc) (ic, ni, 1);

  nvdbg("faked-up node %p for %s\n", ni,
        ieee80211_addr2str((uint8_t *) wh->i_addr2));

  return ieee80211_ref_node(ni);
}

struct ieee80211_node *ieee80211_find_node_for_beacon(struct ieee80211_s *ic,
                                                      const uint8_t * macaddr,
                                                      const struct
                                                      ieee80211_channel *chan,
                                                      const char *ssid,
                                                      uint8_t rssi)
{
  struct ieee80211_node *ni, *keep = NULL;
  uip_lock_t flags;
  int score = 0;

  if ((ni = ieee80211_find_node(ic, macaddr)) != NULL)
    {
      flags = uip_lock();

      if (ni->ni_chan != chan && ni->ni_rssi >= rssi)
        score++;
      if (ssid[1] == 0 && ni->ni_esslen != 0)
        score++;
      if (score > 0)
        keep = ni;

      uip_unlock(flags);
    }

  return (keep);
}

void ieee80211_free_node(struct ieee80211_s *ic, struct ieee80211_node *ni)
{
  DEBUGASSERT(ni != ic->ic_bss);

  nvdbg("%s\n", ieee80211_addr2str(ni->ni_macaddr));
#ifdef CONFIG_IEEE80211_AP
  wd_cancel(ni->ni_eapol_to);
  wd_cancel(ni->ni_sa_query_to);
  IEEE80211_AID_CLR(ni->ni_associd, ic->ic_aid_bitmap);
#endif
  RB_REMOVE(ieee80211_tree, &ic->ic_tree, ni);
  ic->ic_nnodes--;

#ifdef CONFIG_IEEE80211_AP
  if (!IOB_QEMPTY(&ni->ni_savedq))
    {
      iob_free_queue(&ni->ni_savedq);
      if (ic->ic_set_tim != NULL)
        {
          (*ic->ic_set_tim) (ic, ni->ni_associd, 0);
        }
    }
#endif

  (*ic->ic_node_free) (ic, ni);

  /* TBD indicate to drivers that a new node can be allocated */
}

void ieee80211_release_node(struct ieee80211_s *ic, struct ieee80211_node *ni)
{
  uip_lock_t flags;

  nvdbg("%s refcnt %u\n", ieee80211_addr2str(ni->ni_macaddr), ni->ni_refcnt);
  flags = uip_lock();
  if (ieee80211_node_decref(ni) == 0 && ni->ni_state == IEEE80211_STA_COLLECT)
    {
      ieee80211_free_node(ic, ni);
    }
  uip_unlock(flags);
}

void ieee80211_free_allnodes(struct ieee80211_s *ic)
{
  struct ieee80211_node *ni;
  uip_lock_t flags;

  nvdbg("freeing all nodes\n");
  flags = uip_lock();
  while ((ni = RB_MIN(ieee80211_tree, &ic->ic_tree)) != NULL)
    ieee80211_free_node(ic, ni);
  uip_unlock(flags);

  if (ic->ic_bss != NULL)
    ieee80211_node_cleanup(ic, ic->ic_bss);     /* for station mode */
}

/* Timeout inactive nodes.
 *
 * If called because of a cache timeout, which happens only in hostap and ibss
 * modes, clean all inactive cached or authenticated nodes but don't de-auth
 * any associated nodes.
 *
 * Else, this function is called because a new node must be allocated but the
 * node cache is full. In this case, return as soon as a free slot was made
 * available. If acting as hostap, clean cached nodes regardless of their
 * recent activity and also allow de-authing of authenticated nodes older
 * than one cache wait interval, and de-authing of inactive associated nodes.
 */

void ieee80211_clean_nodes(struct ieee80211_s *ic, int cache_timeout)
{
  struct ieee80211_node *ni, *next_ni;
  unsigned int gen = ic->ic_scangen++;  /* NB: ok 'cuz single-threaded */
  uip_lock_t flags;
#ifdef CONFIG_IEEE80211_AP
  int nnodes = 0;
#endif

  flags = uip_lock();
  for (ni = RB_MIN(ieee80211_tree, &ic->ic_tree); ni != NULL; ni = next_ni)
    {
      next_ni = RB_NEXT(ieee80211_tree, &ic->ic_tree, ni);
      if (!cache_timeout && ic->ic_nnodes < ic->ic_max_nnodes)
        break;
      if (ni->ni_scangen == gen)        /* previously handled */
        continue;
#ifdef CONFIG_IEEE80211_AP
      nnodes++;
#endif
      ni->ni_scangen = gen;
      if (ni->ni_refcnt > 0)
        continue;
#ifdef CONFIG_IEEE80211_AP
      if ((ic->ic_opmode == IEEE80211_M_HOSTAP ||
           ic->ic_opmode == IEEE80211_M_IBSS) &&
          ic->ic_state == IEEE80211_S_RUN)
        {
          if (cache_timeout)
            {
              if (ni->ni_state != IEEE80211_STA_COLLECT &&
                  (ni->ni_state == IEEE80211_STA_ASSOC ||
                   ni->ni_inact < IEEE80211_INACT_MAX))
                continue;
            }
          else
            {
              if (ic->ic_opmode == IEEE80211_M_HOSTAP &&
                  ((ni->ni_state == IEEE80211_STA_ASSOC &&
                    ni->ni_inact < IEEE80211_INACT_MAX) ||
                   (ni->ni_state == IEEE80211_STA_AUTH && ni->ni_inact == 0)))
                continue;

              if (ic->ic_opmode == IEEE80211_M_IBSS &&
                  ni->ni_state != IEEE80211_STA_COLLECT &&
                  ni->ni_state != IEEE80211_STA_CACHE &&
                  ni->ni_inact < IEEE80211_INACT_MAX)
                continue;
            }
        }

      nvdbg("%s: station %s purged from node cache\n",
            ic->ic_ifname, ieee80211_addr2str(ni->ni_macaddr));
#endif
      /* If we're hostap and the node is authenticated, send a deauthentication 
       * frame. The node will be freed when the driver calls
       * ieee80211_release_node().
       */

#ifdef CONFIG_IEEE80211_AP
      nnodes--;
      if (ic->ic_opmode == IEEE80211_M_HOSTAP &&
          ni->ni_state >= IEEE80211_STA_AUTH &&
          ni->ni_state != IEEE80211_STA_COLLECT)
        {
          uip_unlock(flags);
          IEEE80211_SEND_MGMT(ic, ni,
                              IEEE80211_FC0_SUBTYPE_DEAUTH,
                              IEEE80211_REASON_AUTH_EXPIRE);
          flags = uip_lock();
          ieee80211_node_leave(ic, ni);
        }
      else
#endif
        {
          ieee80211_free_node(ic, ni);
        }
    }

#ifdef CONFIG_IEEE80211_AP
  /* During a cache timeout we iterate over all nodes. Check for node leaks by
   * comparing the actual number of cached nodes with the ic_nnodes count,
   * which is maintained while adding and removing nodes from the cache.
   */

  if (cache_timeout && nnodes != ic->ic_nnodes)
    {
      ndbg
        ("WARNING: %s: number of cached nodes is %d, expected %d, possible nodes leak\n",
         ic->ic_ifname, nnodes, ic->ic_nnodes);
    }
#endif
  uip_unlock(flags);
}

void ieee80211_iterate_nodes(struct ieee80211_s *ic, ieee80211_iter_func * f,
                             void *arg)
{
  struct ieee80211_node *ni;
  uip_lock_t s;

  flags = uip_lock();
  RB_FOREACH(ni, ieee80211_tree, &ic->ic_tree) (*f) (arg, ni);
  uip_unlock(flags);
}

/* Install received rate set information in the node's state block */

int ieee80211_setup_rates(struct ieee80211_s *ic, struct ieee80211_node *ni,
                          const uint8_t * rates, const uint8_t * xrates,
                          int flags)
{
  struct ieee80211_rateset *rs = &ni->ni_rates;

  memset(rs, 0, sizeof(*rs));
  rs->rs_nrates = rates[1];
  memcpy(rs->rs_rates, rates + 2, rs->rs_nrates);
  if (xrates != NULL)
    {
      uint8_t nxrates;

      /* Tack on 11g extended supported rate element. */

      nxrates = xrates[1];
      if (rs->rs_nrates + nxrates > IEEE80211_RATE_MAXSIZE)
        {
          nxrates = IEEE80211_RATE_MAXSIZE - rs->rs_nrates;
          ndbg
            ("ERROR: extended rate set too large; only using %u of %u rates\n",
             nxrates, xrates[1]);
        }

      memcpy(rs->rs_rates + rs->rs_nrates, xrates + 2, nxrates);
      rs->rs_nrates += nxrates;
    }
  return ieee80211_fix_rate(ic, ni, flags);
}

#ifdef CONFIG_IEEE80211_AP

/* Check if the specified node supports ERP */

int ieee80211_iserp_sta(const struct ieee80211_node *ni)
{
#  define N(a)    (sizeof (a) / sizeof (a)[0])
  static const uint8_t rates[] = { 2, 4, 11, 22, 12, 24, 48 };
  const struct ieee80211_rateset *rs = &ni->ni_rates;
  int i, j;

  /* A STA supports ERP operation if it includes all the Clause 19 mandatory
   * rates in its supported rate set.
   */

  for (i = 0; i < N(rates); i++)
    {
      for (j = 0; j < rs->rs_nrates; j++)
        {
          if ((rs->rs_rates[j] & IEEE80211_RATE_VAL) == rates[i])
            break;
        }
      if (j == rs->rs_nrates)
        return 0;
    }

  return 1;
#  undef N
}

/* This function is called to notify the 802.1X PACP machine that a new
 * 802.1X port is enabled and must be authenticated. For 802.11, a port
 * becomes enabled whenever a STA successfully completes Open System
 * authentication with an AP.
 */

void ieee80211_needs_auth(struct ieee80211_s *ic, struct ieee80211_node *ni)
{
  /* XXX this could be done via the route socket of via a dedicated EAP socket
   * or another kernel->userland notification mechanism. The notification
   * should include the MAC address (ni_macaddr).
   */
}

#  ifdef CONFIG_IEEE80211_HT

/* Handle an HT STA joining an HT network */

void ieee80211_node_join_ht(struct ieee80211_s *ic, struct ieee80211_node *ni)
{
  /* TBD */
}
#  endif /* !CONFIG_IEEE80211_HT */

/* Handle a station joining an RSN network */

void ieee80211_node_join_rsn(struct ieee80211_s *ic, struct ieee80211_node *ni)
{
  nvdbg
    ("station %s associated using proto %d akm 0x%x cipher 0x%x groupcipher 0x%x\n",
     ieee80211_addr2str(ni->ni_macaddr), ni->ni_rsnprotos, ni->ni_rsnakms,
     ni->ni_rsnciphers, ni->ni_rsngroupcipher);

  ni->ni_rsn_state = RSNA_AUTHENTICATION;
  ic->ic_rsnsta++;

  ni->ni_key_count = 0;
  ni->ni_port_valid = 0;
  ni->ni_flags &= ~IEEE80211_NODE_TXRXPROT;
  ni->ni_replaycnt = -1;        /* XXX */
  ni->ni_rsn_retries = 0;
  ni->ni_rsncipher = ni->ni_rsnciphers;

  ni->ni_rsn_state = RSNA_AUTHENTICATION_2;

  /* generate a new authenticator nonce (ANonce) */

  arc4random_buf(ni->ni_nonce, EAPOL_KEY_NONCE_LEN);

  if (!ieee80211_is_8021x_akm(ni->ni_rsnakms))
    {
      memcpy(ni->ni_pmk, ic->ic_psk, IEEE80211_PMK_LEN);
      ni->ni_flags |= IEEE80211_NODE_PMK;
      (void)ieee80211_send_4way_msg1(ic, ni);
    }
  else if (ni->ni_flags & IEEE80211_NODE_PMK)
    {
      /* skip 802.1X auth if a cached PMK was found */

      (void)ieee80211_send_4way_msg1(ic, ni);
    }
  else
    {
      /* no cached PMK found, needs full 802.1X auth */

      ieee80211_needs_auth(ic, ni);
    }
}

/* Handle a station joining an 11g network */

void ieee80211_node_join_11g(struct ieee80211_s *ic, struct ieee80211_node *ni)
{
  if (!(ni->ni_capinfo & IEEE80211_CAPINFO_SHORT_SLOTTIME))
    {
      /* Joining STA doesn't support short slot time.  We must disable the use
       * of short slot time for all other associated STAs and give the driver a 
       * chance to reconfigure the hardware.
       */

      if (++ic->ic_longslotsta == 1)
        {
          if (ic->ic_caps & IEEE80211_C_SHSLOT)
            ieee80211_set_shortslottime(ic, 0);
        }
      nvdbg("[%s] station needs long slot time, count %d\n",
            ieee80211_addr2str(ni->ni_macaddr), ic->ic_longslotsta);
    }

  if (!ieee80211_iserp_sta(ni))
    {
      /* Joining STA is non-ERP. */

      ic->ic_nonerpsta++;

      nvdbg("[%s] station is non-ERP, %d non-ERP stations associated\n",
            ieee80211_addr2str(ni->ni_macaddr), ic->ic_nonerpsta);

      /* must enable the use of protection */

      if (ic->ic_protmode != IEEE80211_PROT_NONE)
        {
          nvdbg("enable use of protection\n");
          ic->ic_flags |= IEEE80211_F_USEPROT;
        }

      if (!(ni->ni_capinfo & IEEE80211_CAPINFO_SHORT_PREAMBLE))
        ic->ic_flags &= ~IEEE80211_F_SHPREAMBLE;
    }
  else
    ni->ni_flags |= IEEE80211_NODE_ERP;
}

void ieee80211_node_join(struct ieee80211_s *ic, struct ieee80211_node *ni,
                         int resp)
{
  int newassoc;

  if (ni->ni_associd == 0)
    {
      uint16_t aid;

      /* It would be clever to search the bitmap more efficiently, but this
       * will do for now.
       */

      for (aid = 1; aid < ic->ic_max_aid; aid++)
        {
          if (!IEEE80211_AID_ISSET(aid, ic->ic_aid_bitmap))
            break;
        }
      if (aid >= ic->ic_max_aid)
        {
          IEEE80211_SEND_MGMT(ic, ni, resp, IEEE80211_REASON_ASSOC_TOOMANY);
          ieee80211_node_leave(ic, ni);
          return;
        }

      ni->ni_associd = aid | 0xc000;
      IEEE80211_AID_SET(ni->ni_associd, ic->ic_aid_bitmap);
      newassoc = 1;
      if (ic->ic_curmode == IEEE80211_MODE_11G)
        ieee80211_node_join_11g(ic, ni);
    }
  else
    newassoc = 0;

  nvdbg("station %s %s associated at aid %d\n",
        ieee80211_addr2str(ni->ni_macaddr), newassoc ? "newly" : "already",
        ni->ni_associd & ~0xc000);

  /* give driver a chance to setup state like ni_txrate */

  if (ic->ic_newassoc)
    (*ic->ic_newassoc) (ic, ni, newassoc);

  IEEE80211_SEND_MGMT(ic, ni, resp, IEEE80211_STATUS_SUCCESS);
  ieee80211_node_newstate(ni, IEEE80211_STA_ASSOC);

  if (!(ic->ic_flags & IEEE80211_F_RSNON))
    {
      ni->ni_port_valid = 1;
      ni->ni_rsncipher = IEEE80211_CIPHER_USEGROUP;
    }
  else
    ieee80211_node_join_rsn(ic, ni);

#  ifdef CONFIG_IEEE80211_HT
  if (ni->ni_flags & IEEE80211_NODE_HT)
    ieee80211_node_join_ht(ic, ni);
#  endif

#  ifdef CONFIG_IEEE80211_BRIDGEPORT
  /* If the parent interface is a bridgeport, learn the node's address
   * dynamically on this interface.
   */

  if (ic->ic_bridgeport != NULL)
    {
      bridge_update(ic, (struct ether_addr *)ni->ni_macaddr, 0);
    }
#  endif
}

#  ifdef CONFIG_IEEE80211_HT

/* Handle an HT STA leaving an HT network */

void ieee80211_node_leave_ht(struct ieee80211_s *ic, struct ieee80211_node *ni)
{
  struct ieee80211_rx_ba *ba;
  uint8_t tid;
  int i;

  /* Free all Block Ack records */

  for (tid = 0; tid < IEEE80211_NUM_TID; tid++)
    {
      ba = &ni->ni_rx_ba[tid];
      if (ba->ba_buf != NULL)
        {
          for (i = 0; i < IEEE80211_BA_MAX_WINSZ; i++)
            {
              if (ba->ba_buf[i].iob != NULL)
                {
                  iob_free_chain(ba->ba_buf[i].iob);
                }
            }

          kfree(ba->ba_buf);
          ba->ba_buf = NULL;
        }
    }
}
#  endif                               /* !CONFIG_IEEE80211_HT */

/* Handle a station leaving an RSN network */

void ieee80211_node_leave_rsn(struct ieee80211_s *ic, struct ieee80211_node *ni)
{
  ni->ni_rsn_state = RSNA_DISCONNECTED;
  ic->ic_rsnsta--;

  ni->ni_rsn_state = RSNA_INITIALIZE;
  if ((ni->ni_flags & IEEE80211_NODE_REKEY) && --ic->ic_rsn_keydonesta == 0)
    ieee80211_setkeysdone(ic);
  ni->ni_flags &= ~IEEE80211_NODE_REKEY;

  ni->ni_flags &= ~IEEE80211_NODE_PMK;
  ni->ni_rsn_gstate = RSNA_IDLE;

  wd_cancel(ni->ni_eapol_to);
  wd_cancel(ni->ni_sa_query_to);

  ni->ni_rsn_retries = 0;
  ni->ni_flags &= ~IEEE80211_NODE_TXRXPROT;
  ni->ni_port_valid = 0;
  (*ic->ic_delete_key) (ic, ni, &ni->ni_pairwise_key);
}

/* Handle a station leaving an 11g network */

void ieee80211_node_leave_11g(struct ieee80211_s *ic, struct ieee80211_node *ni)
{
  if (!(ni->ni_capinfo & IEEE80211_CAPINFO_SHORT_SLOTTIME))
    {
      DEBUGASSERT(ic->ic_longslotsta != 0);

      /* leaving STA did not support short slot time */
      if (--ic->ic_longslotsta == 0)
        {
          /* All associated STAs now support short slot time, so enable this
           * feature and give the driver a chance to reconfigure the hardware.
           * Notice that IBSS always use a long slot time.
           */

          if ((ic->ic_caps & IEEE80211_C_SHSLOT) &&
              ic->ic_opmode != IEEE80211_M_IBSS)
            {
              ieee80211_set_shortslottime(ic, 1);
            }
        }

      nvdbg("[%s] long slot time station leaves, count %d\n",
            ieee80211_addr2str(ni->ni_macaddr), ic->ic_longslotsta);
    }

  if (!(ni->ni_flags & IEEE80211_NODE_ERP))
    {
      DEBUGASSERT(ic->ic_nonerpsta != 0);
      /* leaving STA was non-ERP */
      if (--ic->ic_nonerpsta == 0)
        {
          /* All associated STAs are now ERP capable, disable use of protection 
           * and re-enable short preamble support.
           */

          ic->ic_flags &= ~IEEE80211_F_USEPROT;
          if (ic->ic_caps & IEEE80211_C_SHPREAMBLE)
            {
              ic->ic_flags |= IEEE80211_F_SHPREAMBLE;
            }
        }

      nvdbg("[%s] non-ERP station leaves, count %d\n",
            ieee80211_addr2str(ni->ni_macaddr), ic->ic_nonerpsta);
    }
}

/* Handle bookkeeping for station deauthentication/disassociation
 * when operating as an ap.
 */

void ieee80211_node_leave(struct ieee80211_s *ic, struct ieee80211_node *ni)
{
  DEBUGASSERT(ic->ic_opmode == IEEE80211_M_HOSTAP);

  /* If node wasn't previously associated all we need to do is reclaim the
   * reference.
   */

  if (ni->ni_associd == 0)
    {
      ieee80211_node_newstate(ni, IEEE80211_STA_COLLECT);
      return;
    }

  if (ni->ni_pwrsave == IEEE80211_PS_DOZE)
    {
      ic->ic_pssta--;
      ni->ni_pwrsave = IEEE80211_PS_AWAKE;
    }

  if (!IOB_QEMPTY(&ni->ni_savedq))
    {
      iob_free_queue(&ni->ni_savedq);
      if (ic->ic_set_tim != NULL)
        {
          (*ic->ic_set_tim) (ic, ni->ni_associd, 0);
        }
    }

  if (ic->ic_flags & IEEE80211_F_RSNON)
    {
      ieee80211_node_leave_rsn(ic, ni);
    }

  if (ic->ic_curmode == IEEE80211_MODE_11G)
    {
      ieee80211_node_leave_11g(ic, ni);
    }

#  ifdef CONFIG_IEEE80211_HT
  if (ni->ni_flags & IEEE80211_NODE_HT)
    {
      ieee80211_node_leave_ht(ic, ni);
    }
#  endif

  if (ic->ic_node_leave != NULL)
    {
      (*ic->ic_node_leave) (ic, ni);
    }

  IEEE80211_AID_CLR(ni->ni_associd, ic->ic_aid_bitmap);
  ni->ni_associd = 0;
  ieee80211_node_newstate(ni, IEEE80211_STA_COLLECT);
#  ifdef CONFIG_IEEE80211_BRIDGEPORT

  /* If the parent interface is a bridgeport, delete any dynamically learned
   * address for this node.
   */

  if (ic->ic_bridgeport != NULL)
    {
      bridge_update(ic, (struct ether_addr *)ni->ni_macaddr, 1);
    }
#  endif
}

static int ieee80211_do_slow_print(struct ieee80211_s *ic, bool * did_print)
{
  static const struct timeval merge_print_intvl = {
    1, 0
  };
  if (!*did_print && !ratecheck(&ic->ic_last_merge_print, &merge_print_intvl))
    {
      return false;
    }

  *did_print = true;
  return 1;
}

/* ieee80211_ibss_merge helps merge 802.11 ad hoc networks.  The
 * convention, set by the Wireless Ethernet Compatibility Alliance
 * (WECA), is that an 802.11 station will change its BSSID to match
 * the "oldest" 802.11 ad hoc network, on the same channel, that
 * has the station's desired SSID.  The "oldest" 802.11 network
 * sends beacons with the greatest TSF timestamp.
 *
 * Return ENETRESET if the BSSID changed, 0 otherwise.
 *
 * XXX Perhaps we should compensate for the time that elapses
 * between the MAC receiving the beacon and the host processing it
 * in ieee80211_ibss_merge.
 */

int ieee80211_ibss_merge(struct ieee80211_s *ic,
                         struct ieee80211_node *ni, uint64_t local_tsft)
{
  uint64_t beacon_tsft;
  int sign;
  bool did_print = false;
  union
    {
      uint64_t word;
      uint8_t tstamp[8];
    } u;

  /* Ensure alignment */

  (void)memcpy(&u, &ni->ni_tstamp[0], sizeof(u));
  beacon_tsft = letoh64(u.word);

  /* Ee are faster, let the other guy catch up */

  if (beacon_tsft < local_tsft)
    {
      sign = -1;
    }
  else
    {
      sign = 1;
    }

  if (IEEE80211_ADDR_EQ(ni->ni_bssid, ic->ic_bss->ni_bssid))
    {
      if (!ieee80211_do_slow_print(ic, &did_print))
        {
          return 0;
        }

      nvdbg("%s: tsft offset %s%llu\n", ic->ic_ifname,
            (sign < 0) ? "-" : "",
            (sign < 0)
            ? (local_tsft - beacon_tsft) : (beacon_tsft - local_tsft));
      return 0;
    }

  if (sign < 0)
    {
      return 0;
    }

  if (ieee80211_match_bss(ic, ni) != 0)
    {
      return 0;
    }

  if (ieee80211_do_slow_print(ic, &did_print))
    {
      nvdbg("%s: ieee80211_ibss_merge: bssid mismatch %s\n",
            ic->ic_ifname, ieee80211_addr2str(ni->ni_bssid));
      nvdbg("%s: my tsft %llu beacon tsft %llu\n",
            ic->ic_ifname, local_tsft, beacon_tsft);
      nvdbg("%s: sync TSF with %s\n",
            ic->ic_ifname, ieee80211_addr2str(ni->ni_macaddr));
    }

  ic->ic_flags &= ~IEEE80211_F_SIBSS;

  /* negotiate rates with new IBSS */

  ieee80211_fix_rate(ic, ni,
                     IEEE80211_F_DOFRATE | IEEE80211_F_DONEGO |
                     IEEE80211_F_DODEL);
  if (ni->ni_rates.rs_nrates == 0)
    {
      if (ieee80211_do_slow_print(ic, &did_print))
        {
          nvdbg("%s: rates mismatch, BSSID %s\n",
                ic->ic_ifname, ieee80211_addr2str(ni->ni_bssid));
        }

      return 0;
    }

  if (ieee80211_do_slow_print(ic, &did_print))
    {
      nvdbg("%s: sync BSSID %s -> ",
            ic->ic_ifname, ieee80211_addr2str(ic->ic_bss->ni_bssid));
      nvdbg("%s ", ieee80211_addr2str(ni->ni_bssid));
      nvdbg("(from %s)\n", ieee80211_addr2str(ni->ni_macaddr));
    }

  ieee80211_node_newstate(ni, IEEE80211_STA_BSS);
  (*ic->ic_node_copy) (ic, ic->ic_bss, ni);
  return -ENETRESET;
}

void ieee80211_set_tim(struct ieee80211_s *ic, int aid, int set)
{
  int ndx;
  int bit;
  aid &= 0xc000;
  ndx = (aid >> 3);
  bit = (aid & 7);
  if (set)
    {
      ic->ic_tim_bitmap[ndx] |= (1 << bit);
    }
  else
    {
      ic->ic_tim_bitmap[ndx] &= ~(1 << bit);
    }
}

/* This function shall be called by drivers immediately after every DTIM.
 * Transmit all group addressed MSDUs buffered at the AP.
 */

void ieee80211_notify_dtim(struct ieee80211_s *ic)
{
  /* NB: group addressed MSDUs are buffered in ic_bss */

  struct ieee80211_node *ni = ic->ic_bss;
  struct ieee80211_frame *wh;
  struct iob_s *iob;
  DEBUGASSERT(ic->ic_opmode == IEEE80211_M_HOSTAP);
  for (;;)
    {
      iob = iob_remove_queue(&ni->ni_savedq);
      if (iob == NULL)
        {
          break;
        }

      if (!IOB_QEMPTY(&ni->ni_savedq))
        {
          /* more queued frames, set the more data bit */

          wh = (FAR struct ieee80211_frame *)IOB_DATA(iob);
          wh->i_fc[1] |= IEEE80211_FC1_MORE_DATA;
        }

      iob_add_queue(iob, &ic->ic_pwrsaveq);
    }

  /* XXX assumes everything has been sent */

  ic->ic_tim_mcast_pending = 0;
}
#endif /* CONFIG_IEEE80211_AP */

/* Compare nodes in the tree by lladd */

int ieee80211_node_cmp(const struct ieee80211_node *b1,
                       const struct ieee80211_node *b2)
{
  return (memcmp(b1->ni_macaddr, b2->ni_macaddr, IEEE80211_ADDR_LEN));
}

/* Generate red-black tree function logic */

RB_GENERATE(ieee80211_tree, ieee80211_node, ni_node, ieee80211_node_cmp);
