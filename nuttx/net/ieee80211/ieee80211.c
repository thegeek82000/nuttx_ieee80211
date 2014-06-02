/****************************************************************************
 * net/ieee80211/i33380211.c
 * IEEE 802.11 generic handler
 *
 * Copyright (c) 2001 Atsushi Onoe
 * Copyright (c) 2002, 2003 Sam Leffler, Errno Consulting
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

#include <sys/socket.h>
#include <sys/sockio.h>

#include <string.h>
#include <queue.h>
#include <errno.h>
#include <assert.h>
#include <debug.h>

#include <net/if.h>

#ifdef CONFIG_NET_ETHERNET
#  include <netinet/in.h>
#  include <nuttx/net/uip/uip.h>
#endif

#include <nuttx/net/ieee80211/ieee80211_ifnet.h>
#include <nuttx/net/ieee80211/ieee80211_var.h>
#include <nuttx/net/ieee80211/ieee80211_priv.h>

int ieee80211_cache_size = IEEE80211_CACHE_SIZE;

dq_queue_t ieee80211com_head;

void ieee80211_setbasicrates(struct ieee80211com *);
int ieee80211_findrate(struct ieee80211com *, enum ieee80211_phymode, int);

#warning REVISIT:  There is no concept of attaching devices in NuttX.
#warning REVISIT:  Perhaps this should become an general one-time initialization function
#warning REVISIT:  This should receive the internet string as an argument ("eth0").
void ieee80211_ifattach(struct ieee80211com *ic)
{
  struct ieee80211_channel *c;
  int ndx;
  int bit;
  int i;

  ieee80211_crypto_attach(ic);

  /* Fill in 802.11 available channel set, mark all available channels as
   * active, and pick a default channel if not already specified.
   */

  memset(ic->ic_chan_avail, 0, sizeof(ic->ic_chan_avail));
  ic->ic_modecaps |= 1<<IEEE80211_MODE_AUTO;
  for (i = 0; i <= IEEE80211_CHAN_MAX; i++)
    {
      c = &ic->ic_channels[i];
      if (c->ic_flags)
        {
          /* Verify driver passed us valid data */

          if (i != ieee80211_chan2ieee(ic, c))
            {
              nvdbg("ERROR %s: bad channel ignored; freq %u flags %x number %u\n",
                  ic->ic_ifname, c->ic_freq, c->ic_flags, i);

              c->ic_flags = 0;    /* NB: remove */
              continue;
            }

          ndx = (i >> 3);
          bit = (i & 7);
          ic->ic_chan_avail[ndx] |= (1 << bit);

          /* Identify mode capabilities */

          if (IEEE80211_IS_CHAN_A(c))
            {
              ic->ic_modecaps |= 1<<IEEE80211_MODE_11A;
            }

          if (IEEE80211_IS_CHAN_B(c))
            {
              ic->ic_modecaps |= 1<<IEEE80211_MODE_11B;
            }

          if (IEEE80211_IS_CHAN_PUREG(c))
            {
              ic->ic_modecaps |= 1<<IEEE80211_MODE_11G;
            }

          if (IEEE80211_IS_CHAN_T(c))
              ic->ic_modecaps |= 1<<IEEE80211_MODE_TURBO;
            }
        }
    }

    /* validate ic->ic_curmode */

    if ((ic->ic_modecaps & (1<<ic->ic_curmode)) == 0)
        ic->ic_curmode = IEEE80211_MODE_AUTO;
    ic->ic_des_chan = IEEE80211_CHAN_ANYC;    /* any channel is ok */
    ic->ic_scan_lock = IEEE80211_SCAN_UNLOCKED;

    /* IEEE 802.11 defines a MTU >= 2290 */

    ieee80211_setbasicrates(ic);
    (void)ieee80211_setmode(ic, ic->ic_curmode);

    if (ic->ic_lintval == 0)
        ic->ic_lintval = 100;        /* default sleep */
    ic->ic_bmisstimeout = 7*ic->ic_lintval;    /* default 7 beacons */
    ic->ic_dtim_period = 1;    /* all TIMs are DTIMs */

    dq_addfirst((FAR dq_entry_t *)ic, &ieee80211com_head);
    ieee80211_node_attach(ic);
    ieee80211_proto_attach(ic);
}

void ieee80211_ifdetach(struct ieee80211com *ic)
{
  ieee80211_proto_detach(ic);
  ieee80211_crypto_detach(ic);
  ieee80211_node_detach(ic);
  dq_rem((dq_entry_t *)ic, &ic_list);
  ifmedia_delete_instance(&ic->ic_media, IFM_INST_ANY);
  ether_ifdetach(ic);
}

/* Convert MHz frequency to IEEE channel number */

unsigned int ieee80211_mhz2ieee(unsigned int freq, unsigned int flags)
{
    if (flags & IEEE80211_CHAN_2GHZ) {    /* 2GHz band */
        if (freq == 2484)
            return 14;
        if (freq < 2484)
            return (freq - 2407) / 5;
        else
            return 15 + ((freq - 2512) / 20);
    } else if (flags & IEEE80211_CHAN_5GHZ) {    /* 5GHz band */
        return (freq - 5000) / 5;
    } else {                /* either, guess */
        if (freq == 2484)
            return 14;
        if (freq < 2484)
            return (freq - 2407) / 5;
        if (freq < 5000)
            return 15 + ((freq - 2512) / 20);
        return (freq - 5000) / 5;
    }
}

/* Convert channel to IEEE channel number */

unsigned int ieee80211_chan2ieee(struct ieee80211com *ic, const struct ieee80211_channel *c)
{
  if (ic->ic_channels <= c && c <= &ic->ic_channels[IEEE80211_CHAN_MAX])
    {
      return c - ic->ic_channels;
    }
  else if (c == IEEE80211_CHAN_ANYC)
    {
      return IEEE80211_CHAN_ANY;
    }
  else if (c != NULL)
    {
      ndbg("ERROR: %s: invalid channel freq %u flags %x\n",
           ic->ic_ifname, c->ic_freq, c->ic_flags);
      return 0;
    }
  else
    {
      ndbg("ERROR: %s: invalid channel (NULL)\n", ic->ic_ifname);
      return 0;
    }
}

/* Convert IEEE channel number to MHz frequency */

unsigned int ieee80211_ieee2mhz(unsigned int chan, unsigned int flags)
{
    if (flags & IEEE80211_CHAN_2GHZ) {    /* 2GHz band */
        if (chan == 14)
            return 2484;
        if (chan < 14)
            return 2407 + chan*5;
        else
            return 2512 + ((chan-15)*20);
    } else if (flags & IEEE80211_CHAN_5GHZ) {/* 5GHz band */
        return 5000 + (chan*5);
    } else {                /* either, guess */
        if (chan == 14)
            return 2484;
        if (chan < 14)            /* 0-13 */
            return 2407 + chan*5;
        if (chan < 27)            /* 15-26 */
            return 2512 + ((chan-15)*20);
        return 5000 + (chan*5);
    }
}

/* Setup the media data structures according to the channel and
 * rate tables.  This must be called by the driver after
 * ieee80211_attach and before most anything else.
 */

void ieee80211_media_init(struct ieee80211com *ic, ifm_change_cb_t media_change, ifm_stat_cb_t media_stat)
{
#define ADD(_ic, _s, _o) \
    ifmedia_add(&(_ic)->ic_media, \
        IFM_MAKEWORD(IFM_IEEE80211, (_s), (_o), 0), 0, NULL)
    struct ifmediareq imr;
    int i, j, mode, rate, maxrate, mword, mopt, r;
    const struct ieee80211_rateset *rs;
    struct ieee80211_rateset allrates;

    /* Do late attach work that must wait for any subclass
     * (i.e. driver) work such as overriding methods.
     */

     ieee80211_node_lateattach(ic);

    /* Fill in media characteristics */

    ifmedia_init(&ic->ic_media, 0, media_change, media_stat);
    maxrate = 0;
    memset(&allrates, 0, sizeof(allrates));
    for (mode = IEEE80211_MODE_AUTO; mode < IEEE80211_MODE_MAX; mode++) {
        static const unsigned int mopts[] = {
            IFM_AUTO,
            IFM_IEEE80211_11A,
            IFM_IEEE80211_11B,
            IFM_IEEE80211_11G,
            IFM_IEEE80211_11A | IFM_IEEE80211_TURBO,
        };
        if ((ic->ic_modecaps & (1<<mode)) == 0)
            continue;
        mopt = mopts[mode];
        ADD(ic, IFM_AUTO, mopt);    /* e.g. 11a auto */
#ifdef CONFIG_IEEE80211_AP
        if (ic->ic_caps & IEEE80211_C_IBSS)
            ADD(ic, IFM_AUTO, mopt | IFM_IEEE80211_IBSS);
        if (ic->ic_caps & IEEE80211_C_HOSTAP)
            ADD(ic, IFM_AUTO, mopt | IFM_IEEE80211_HOSTAP);
        if (ic->ic_caps & IEEE80211_C_AHDEMO)
            ADD(ic, IFM_AUTO, mopt | IFM_IEEE80211_ADHOC);
#endif
        if (ic->ic_caps & IEEE80211_C_MONITOR)
            ADD(ic, IFM_AUTO, mopt | IFM_IEEE80211_MONITOR);
        if (mode == IEEE80211_MODE_AUTO)
            continue;
        rs = &ic->ic_sup_rates[mode];
        for (i = 0; i < rs->rs_nrates; i++)
          {
            rate = rs->rs_rates[i];
            mword = ieee80211_rate2media(ic, rate, mode);
            if (mword == 0)
                continue;
            ADD(ic, mword, mopt);
#ifdef CONFIG_IEEE80211_AP
            if (ic->ic_caps & IEEE80211_C_IBSS)
                ADD(ic, mword, mopt | IFM_IEEE80211_IBSS);
            if (ic->ic_caps & IEEE80211_C_HOSTAP)
                ADD(ic, mword, mopt | IFM_IEEE80211_HOSTAP);
            if (ic->ic_caps & IEEE80211_C_AHDEMO)
                ADD(ic, mword, mopt | IFM_IEEE80211_ADHOC);
#endif
            if (ic->ic_caps & IEEE80211_C_MONITOR)
                ADD(ic, mword, mopt | IFM_IEEE80211_MONITOR);
            /*
             * Add rate to the collection of all rates.
             */
            r = rate & IEEE80211_RATE_VAL;
            for (j = 0; j < allrates.rs_nrates; j++)
                if (allrates.rs_rates[j] == r)
                    break;
            if (j == allrates.rs_nrates) {
                /* unique, add to the set */
                allrates.rs_rates[j] = r;
                allrates.rs_nrates++;
            }
            rate = (rate & IEEE80211_RATE_VAL) / 2;
            if (rate > maxrate)
                maxrate = rate;
        }
    }

    for (i = 0; i < allrates.rs_nrates; i++) {
        mword = ieee80211_rate2media(ic, allrates.rs_rates[i],
                IEEE80211_MODE_AUTO);
        if (mword == 0)
            continue;
        mword = IFM_SUBTYPE(mword);    /* remove media options */
        ADD(ic, mword, 0);
#ifdef CONFIG_IEEE80211_AP
        if (ic->ic_caps & IEEE80211_C_IBSS)
            ADD(ic, mword, IFM_IEEE80211_IBSS);
        if (ic->ic_caps & IEEE80211_C_HOSTAP)
            ADD(ic, mword, IFM_IEEE80211_HOSTAP);
        if (ic->ic_caps & IEEE80211_C_AHDEMO)
            ADD(ic, mword, IFM_IEEE80211_ADHOC);
#endif
        if (ic->ic_caps & IEEE80211_C_MONITOR)
            ADD(ic, mword, IFM_IEEE80211_MONITOR);
    }

    ieee80211_media_status(ic, &imr);
    ifmedia_set(&ic->ic_media, imr.ifm_active);
#undef ADD
}

int ieee80211_findrate(struct ieee80211com *ic, enum ieee80211_phymode mode,
    int rate)
{
#define    IEEERATE(_ic,_m,_i) \
    ((_ic)->ic_sup_rates[_m].rs_rates[_i] & IEEE80211_RATE_VAL)
    int i, nrates = ic->ic_sup_rates[mode].rs_nrates;
    for (i = 0; i < nrates; i++)
        if (IEEERATE(ic, mode, i) == rate)
            return i;
    return -1;
#undef IEEERATE
}

/* Handle a media change request */

int ieee80211_media_change(struct ieee80211com *ic)
{
    struct ifmedia_entry *ime;
    enum ieee80211_opmode newopmode;
    enum ieee80211_phymode newphymode;
    int i, j, newrate, error = 0;

    ime = ic->ic_media.ifm_cur;
    /*
     * First, identify the phy mode.
     */
    switch (IFM_MODE(ime->ifm_media)) {
    case IFM_IEEE80211_11A:
        newphymode = IEEE80211_MODE_11A;
        break;
    case IFM_IEEE80211_11B:
        newphymode = IEEE80211_MODE_11B;
        break;
    case IFM_IEEE80211_11G:
        newphymode = IEEE80211_MODE_11G;
        break;
    case IFM_AUTO:
        newphymode = IEEE80211_MODE_AUTO;
        break;
    default:
        return EINVAL;
    }
    /*
     * Turbo mode is an ``option''.  Eventually it
     * needs to be applied to 11g too.
     */
    if (ime->ifm_media & IFM_IEEE80211_TURBO) {
        if (newphymode != IEEE80211_MODE_11A)
            return EINVAL;
        newphymode = IEEE80211_MODE_TURBO;
    }
    /*
     * Validate requested mode is available.
     */
    if ((ic->ic_modecaps & (1<<newphymode)) == 0)
        return EINVAL;

    /*
     * Next, the fixed/variable rate.
     */
    i = -1;
    if (IFM_SUBTYPE(ime->ifm_media) != IFM_AUTO) {
        /*
         * Convert media subtype to rate.
         */
        newrate = ieee80211_media2rate(ime->ifm_media);
        if (newrate == 0)
            return EINVAL;
        /*
         * Check the rate table for the specified/current phy.
         */
        if (newphymode == IEEE80211_MODE_AUTO) {
            /*
             * In autoselect mode search for the rate.
             */
            for (j = IEEE80211_MODE_11A;
                 j < IEEE80211_MODE_MAX; j++) {
                if ((ic->ic_modecaps & (1<<j)) == 0)
                    continue;
                i = ieee80211_findrate(ic, j, newrate);
                if (i != -1) {
                    /* lock mode too */
                    newphymode = j;
                    break;
                }
            }
        } else {
            i = ieee80211_findrate(ic, newphymode, newrate);
        }
        if (i == -1)            /* mode/rate mismatch */
            return EINVAL;
    }
    /* NB: defer rate setting to later */

    /*
     * Deduce new operating mode but don't install it just yet.
     */
#ifdef CONFIG_IEEE80211_AP
    if (ime->ifm_media & IFM_IEEE80211_ADHOC)
        newopmode = IEEE80211_M_AHDEMO;
    else if (ime->ifm_media & IFM_IEEE80211_HOSTAP)
        newopmode = IEEE80211_M_HOSTAP;
    else if (ime->ifm_media & IFM_IEEE80211_IBSS)
        newopmode = IEEE80211_M_IBSS;
    else
#endif
    if (ime->ifm_media & IFM_IEEE80211_MONITOR)
        newopmode = IEEE80211_M_MONITOR;
    else
        newopmode = IEEE80211_M_STA;

#ifdef CONFIG_IEEE80211_AP
    /*
     * Autoselect doesn't make sense when operating as an AP.
     * If no phy mode has been selected, pick one and lock it
     * down so rate tables can be used in forming beacon frames
     * and the like.
     */
    if (newopmode == IEEE80211_M_HOSTAP &&
        newphymode == IEEE80211_MODE_AUTO) {
        for (j = IEEE80211_MODE_11A; j < IEEE80211_MODE_MAX; j++)
            if (ic->ic_modecaps & (1<<j)) {
                newphymode = j;
                break;
            }
    }
#endif

    /*
     * Handle phy mode change.
     */
    if (ic->ic_curmode != newphymode) {        /* change phy mode */
        error = ieee80211_setmode(ic, newphymode);
        if (error != 0)
            return error;
        error = ENETRESET;
    }

    /*
     * Committed to changes, install the rate setting.
     */
    if (ic->ic_fixed_rate != i) {
        ic->ic_fixed_rate = i;            /* set fixed tx rate */
        error = ENETRESET;
    }

    /*
     * Handle operating mode change.
     */
    if (ic->ic_opmode != newopmode) {
        ic->ic_opmode = newopmode;
#ifdef CONFIG_IEEE80211_AP
        switch (newopmode) {
        case IEEE80211_M_AHDEMO:
        case IEEE80211_M_HOSTAP:
        case IEEE80211_M_STA:
        case IEEE80211_M_MONITOR:
            ic->ic_flags &= ~IEEE80211_F_IBSSON;
            break;
        case IEEE80211_M_IBSS:
            ic->ic_flags |= IEEE80211_F_IBSSON;
            break;
        }
#endif
        /*
         * Yech, slot time may change depending on the
         * operating mode so reset it to be sure everything
         * is setup appropriately.
         */
        ieee80211_reset_erp(ic);
        error = ENETRESET;
    }

  return error;
}

void ieee80211_media_status(struct ieee80211com *ic, struct ifmediareq *imr)
{
    const struct ieee80211_node *ni = NULL;

    imr->ifm_status = IFM_AVALID;
    imr->ifm_active = IFM_IEEE80211;
    if (ic->ic_state == IEEE80211_S_RUN &&
        (ic->ic_opmode != IEEE80211_M_STA ||
         !(ic->ic_flags & IEEE80211_F_RSNON) ||
         ic->ic_bss->ni_port_valid))
        imr->ifm_status |= IFM_ACTIVE;
    imr->ifm_active |= IFM_AUTO;
    switch (ic->ic_opmode) {
    case IEEE80211_M_STA:
        ni = ic->ic_bss;
        /* calculate rate subtype */
        imr->ifm_active |= ieee80211_rate2media(ic,
            ni->ni_rates.rs_rates[ni->ni_txrate], ic->ic_curmode);
        break;
#ifdef CONFIG_IEEE80211_AP
    case IEEE80211_M_IBSS:
        imr->ifm_active |= IFM_IEEE80211_IBSS;
        break;
    case IEEE80211_M_AHDEMO:
        imr->ifm_active |= IFM_IEEE80211_ADHOC;
        break;
    case IEEE80211_M_HOSTAP:
        imr->ifm_active |= IFM_IEEE80211_HOSTAP;
        break;
#endif
    case IEEE80211_M_MONITOR:
        imr->ifm_active |= IFM_IEEE80211_MONITOR;
        break;
    default:
        break;
    }
    switch (ic->ic_curmode) {
    case IEEE80211_MODE_11A:
        imr->ifm_active |= IFM_IEEE80211_11A;
        break;
    case IEEE80211_MODE_11B:
        imr->ifm_active |= IFM_IEEE80211_11B;
        break;
    case IEEE80211_MODE_11G:
        imr->ifm_active |= IFM_IEEE80211_11G;
        break;
    case IEEE80211_MODE_TURBO:
        imr->ifm_active |= IFM_IEEE80211_11A
                |  IFM_IEEE80211_TURBO;
        break;
    }
}

void ieee80211_watchdog(struct ieee80211com *ic)
{
  if (ic->ic_mgt_timer && --ic->ic_mgt_timer == 0)
    {
      ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
    }
}

const struct ieee80211_rateset ieee80211_std_rateset_11a =
    { 8, { 12, 18, 24, 36, 48, 72, 96, 108 } };

const struct ieee80211_rateset ieee80211_std_rateset_11b =
    { 4, { 2, 4, 11, 22 } };

const struct ieee80211_rateset ieee80211_std_rateset_11g =
    { 12, { 2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108 } };

/* Mark the basic rates for the 11g rate table based on the
 * operating mode.  For real 11g we mark all the 11b rates
 * and 6, 12, and 24 OFDM.  For 11b compatibility we mark only
 * 11b rates.  There's also a pseudo 11a-mode used to mark only
 * the basic OFDM rates.
 */

void ieee80211_setbasicrates(struct ieee80211com *ic)
{
    static const struct ieee80211_rateset basic[] = {
        { 0 },                /* IEEE80211_MODE_AUTO */
        { 3, { 12, 24, 48 } },        /* IEEE80211_MODE_11A */
        { 2, { 2, 4 } },            /* IEEE80211_MODE_11B */
        { 4, { 2, 4, 11, 22 } },        /* IEEE80211_MODE_11G */
        { 0 },                /* IEEE80211_MODE_TURBO    */
    };
    enum ieee80211_phymode mode;
    struct ieee80211_rateset *rs;
    int i, j;

    for (mode = 0; mode < IEEE80211_MODE_MAX; mode++) {
        rs = &ic->ic_sup_rates[mode];
        for (i = 0; i < rs->rs_nrates; i++) {
            rs->rs_rates[i] &= IEEE80211_RATE_VAL;
            for (j = 0; j < basic[mode].rs_nrates; j++) {
                if (basic[mode].rs_rates[j] ==
                    rs->rs_rates[i]) {
                    rs->rs_rates[i] |=
                        IEEE80211_RATE_BASIC;
                    break;
                }
            }
        }
    }
}

/* Set the current phy mode and recalculate the active channel
 * set based on the available channels for this mode.  Also
 * select a new default/current channel if the current one is
 * inappropriate for this mode.
 */

int ieee80211_setmode(struct ieee80211com *ic, enum ieee80211_phymode mode)
{
#define    N(a)    (sizeof(a) / sizeof(a[0]))
    static const unsigned int chanflags[] = {
        0,            /* IEEE80211_MODE_AUTO */
        IEEE80211_CHAN_A,    /* IEEE80211_MODE_11A */
        IEEE80211_CHAN_B,    /* IEEE80211_MODE_11B */
        IEEE80211_CHAN_PUREG,    /* IEEE80211_MODE_11G */
        IEEE80211_CHAN_T,    /* IEEE80211_MODE_TURBO    */
    };
    const struct ieee80211_channel *c;
    unsigned int modeflags;
    int ibss;
    int ndx;
    int bit;
    int i;

    /* validate new mode */
    if ((ic->ic_modecaps & (1<<mode)) == 0) {
        ndbg("ERROR: mode %u not supported (caps 0x%x)\n", mode, ic->ic_modecaps);
        return EINVAL;
    }

    /* Verify at least one channel is present in the available
     * channel list before committing to the new mode.
     */

    DEBUGASSERT(mode < N(chanflags));

    modeflags = chanflags[mode];
    for (i = 0; i <= IEEE80211_CHAN_MAX; i++) {
        c = &ic->ic_channels[i];
        if (mode == IEEE80211_MODE_AUTO) {
            /* ignore turbo channels for autoselect */
            if ((c->ic_flags &~ IEEE80211_CHAN_TURBO) != 0)
                break;
        } else {
            if ((c->ic_flags & modeflags) == modeflags)
                break;
        }
    }
    if (i > IEEE80211_CHAN_MAX) {
        ndbg("ERROR: no channels found for mode %u\n", mode);
        return EINVAL;
    }

    /* Calculate the active channel set */

    memset(ic->ic_chan_active, 0, sizeof(ic->ic_chan_active));
    for (i = 0; i <= IEEE80211_CHAN_MAX; i++)
      {
        c = &ic->ic_channels[i];
        ndx = (i >> 3);
        bit = (i & 7);

        if (mode == IEEE80211_MODE_AUTO)
          {
            /* Take anything but pure turbo channels */

            if ((c->ic_flags &~ IEEE80211_CHAN_TURBO) != 0)
              {
                ic->ic_chan_active[ndx] |= (1 << bit);
              }
          }
        else if ((c->ic_flags & modeflags) == modeflags)
          {
            ic->ic_chan_active[ndx] |= (1 << bit);
          }
      }

    /* If no current/default channel is setup or the current
     * channel is wrong for the mode then pick the first
     * available channel from the active list.  This is likely
     * not the right one.
     */

    ibss =  ieee80211_chan2ieee(ic, ic->ic_ibss_chan);
    ndx  = (ibss >> 3);
    bit  = (ibss & 7);

    if (ic->ic_ibss_chan == NULL || (ic->ic_chan_active[ndx] & (1 << bit)) == 0)
      {
        for (i = 0; i <= IEEE80211_CHAN_MAX; i++)
          {
            ndx = (i >> 3);
            bit = (i & 7);

            if ((ic->ic_chan_active[ndx] & (1 << i)) != 0)
              {
                ic->ic_ibss_chan = &ic->ic_channels[i];
                break;
              }
          }

        ibss = ieee80211_chan2ieee(ic, ic->ic_ibss_chan);
        ndx  = (ibss >> 3);
        bit  = (ibss & 7);

        if ((ic->ic_ibss_chan == NULL) || (ic->ic_chan_active[ndx] & (1 << bit)) == 0)
          {
            ndbg("ERROR: Bad IBSS channel %u", ibss);
            PANIC();
          }
      }

    /* Reset the scan state for the new mode. This avoids scanning
     * of invalid channels, ie. 5GHz channels in 11b mode.
     */

    ieee80211_reset_scan(ic);

    ic->ic_curmode = mode;
    ieee80211_reset_erp(ic);    /* reset ERP state */

    return 0;
#undef N
}

enum ieee80211_phymode ieee80211_next_mode(struct ieee80211com *ic)
{
  if (IFM_MODE(ic->ic_media.ifm_cur->ifm_media) != IFM_AUTO) {
      /*
       * Reset the scan state and indicate a wrap around
       * if we're running in a fixed, user-specified phy mode.
       */
      ieee80211_reset_scan(ic);
      return (IEEE80211_MODE_AUTO);
  }

    /*
     * Get the next supported mode
     */
    for (++ic->ic_curmode;
        ic->ic_curmode <= IEEE80211_MODE_TURBO;
        ic->ic_curmode++) {
        /* Wrap around and ignore turbo mode */
        if (ic->ic_curmode >= IEEE80211_MODE_TURBO) {
            ic->ic_curmode = IEEE80211_MODE_AUTO;
            break;
        }

        if (ic->ic_modecaps & (1 << ic->ic_curmode))
            break;
    }

    ieee80211_setmode(ic, ic->ic_curmode);

    return (ic->ic_curmode);
}

/*
 * Return the phy mode for with the specified channel so the
 * caller can select a rate set.  This is problematic and the
 * work here assumes how things work elsewhere in this code.
 *
 * XXX never returns turbo modes -dcy
 */

enum ieee80211_phymode ieee80211_chan2mode(struct ieee80211com *ic,
    const struct ieee80211_channel *chan)
{
    /*
     * NB: this assumes the channel would not be supplied to us
     *     unless it was already compatible with the current mode.
     */
    if (ic->ic_curmode != IEEE80211_MODE_AUTO ||
        chan == IEEE80211_CHAN_ANYC)
        return ic->ic_curmode;
    /*
     * In autoselect mode; deduce a mode based on the channel
     * characteristics.  We assume that turbo-only channels
     * are not considered when the channel set is constructed.
     */
    if (IEEE80211_IS_CHAN_T(chan))
        return IEEE80211_MODE_TURBO;
    else if (IEEE80211_IS_CHAN_5GHZ(chan))
        return IEEE80211_MODE_11A;
    else if (chan->ic_flags & (IEEE80211_CHAN_OFDM|IEEE80211_CHAN_DYN))
        return IEEE80211_MODE_11G;
    else
        return IEEE80211_MODE_11B;
}

/* Convert IEEE80211 rate value to ifmedia subtype.
 * ieee80211 rate is in unit of 0.5Mbps.
 */

int ieee80211_rate2media(struct ieee80211com *ic, int rate,
    enum ieee80211_phymode mode)
{
#define    N(a)    (sizeof(a) / sizeof(a[0]))
    static const struct {
        unsigned int    m;    /* rate + mode */
        unsigned int    r;    /* if_media rate */
    } rates[] = {
        {   2 | IFM_IEEE80211_11B, IFM_IEEE80211_DS1 },
        {   4 | IFM_IEEE80211_11B, IFM_IEEE80211_DS2 },
        {  11 | IFM_IEEE80211_11B, IFM_IEEE80211_DS5 },
        {  22 | IFM_IEEE80211_11B, IFM_IEEE80211_DS11 },
        {  44 | IFM_IEEE80211_11B, IFM_IEEE80211_DS22 },
        {  12 | IFM_IEEE80211_11A, IFM_IEEE80211_OFDM6 },
        {  18 | IFM_IEEE80211_11A, IFM_IEEE80211_OFDM9 },
        {  24 | IFM_IEEE80211_11A, IFM_IEEE80211_OFDM12 },
        {  36 | IFM_IEEE80211_11A, IFM_IEEE80211_OFDM18 },
        {  48 | IFM_IEEE80211_11A, IFM_IEEE80211_OFDM24 },
        {  72 | IFM_IEEE80211_11A, IFM_IEEE80211_OFDM36 },
        {  96 | IFM_IEEE80211_11A, IFM_IEEE80211_OFDM48 },
        { 108 | IFM_IEEE80211_11A, IFM_IEEE80211_OFDM54 },
        {   2 | IFM_IEEE80211_11G, IFM_IEEE80211_DS1 },
        {   4 | IFM_IEEE80211_11G, IFM_IEEE80211_DS2 },
        {  11 | IFM_IEEE80211_11G, IFM_IEEE80211_DS5 },
        {  22 | IFM_IEEE80211_11G, IFM_IEEE80211_DS11 },
        {  12 | IFM_IEEE80211_11G, IFM_IEEE80211_OFDM6 },
        {  18 | IFM_IEEE80211_11G, IFM_IEEE80211_OFDM9 },
        {  24 | IFM_IEEE80211_11G, IFM_IEEE80211_OFDM12 },
        {  36 | IFM_IEEE80211_11G, IFM_IEEE80211_OFDM18 },
        {  48 | IFM_IEEE80211_11G, IFM_IEEE80211_OFDM24 },
        {  72 | IFM_IEEE80211_11G, IFM_IEEE80211_OFDM36 },
        {  96 | IFM_IEEE80211_11G, IFM_IEEE80211_OFDM48 },
        { 108 | IFM_IEEE80211_11G, IFM_IEEE80211_OFDM54 },
        /* NB: OFDM72 doesn't really exist so we don't handle it */
    };
    unsigned int mask, i;

    mask = rate & IEEE80211_RATE_VAL;
    switch (mode) {
    case IEEE80211_MODE_11A:
    case IEEE80211_MODE_TURBO:
        mask |= IFM_IEEE80211_11A;
        break;
    case IEEE80211_MODE_11B:
        mask |= IFM_IEEE80211_11B;
        break;
    case IEEE80211_MODE_AUTO:
        /* NB: hack, 11g matches both 11b+11a rates */
        /* FALLTHROUGH */
    case IEEE80211_MODE_11G:
        mask |= IFM_IEEE80211_11G;
        break;
    }
    for (i = 0; i < N(rates); i++)
        if (rates[i].m == mask)
            return rates[i].r;
    return IFM_AUTO;
#undef N
}

int ieee80211_media2rate(int mword)
{
#define    N(a)    (sizeof(a) / sizeof(a[0]))
    int i;
    static const struct {
        int subtype;
        int rate;
    } ieeerates[] = {
        { IFM_AUTO,        -1    },
        { IFM_MANUAL,        0    },
        { IFM_NONE,        0    },
        { IFM_IEEE80211_DS1,    2    },
        { IFM_IEEE80211_DS2,    4    },
        { IFM_IEEE80211_DS5,    11    },
        { IFM_IEEE80211_DS11,    22    },
        { IFM_IEEE80211_DS22,    44    },
        { IFM_IEEE80211_OFDM6,    12    },
        { IFM_IEEE80211_OFDM9,    18    },
        { IFM_IEEE80211_OFDM12,    24    },
        { IFM_IEEE80211_OFDM18,    36    },
        { IFM_IEEE80211_OFDM24,    48    },
        { IFM_IEEE80211_OFDM36,    72    },
        { IFM_IEEE80211_OFDM48,    96    },
        { IFM_IEEE80211_OFDM54,    108    },
        { IFM_IEEE80211_OFDM72,    144    },
    };
    for (i = 0; i < N(ieeerates); i++) {
        if (ieeerates[i].subtype == IFM_SUBTYPE(mword))
            return ieeerates[i].rate;
    }
    return 0;
#undef N
}

/* Convert bit rate (in 0.5Mbps units) to PLCP signal (R4-R1) and vice versa */

uint8_t ieee80211_rate2plcp(uint8_t rate, enum ieee80211_phymode mode)
{
  rate &= IEEE80211_RATE_VAL;

  if (mode == IEEE80211_MODE_11B)
    {
      /* IEEE Std 802.11b-1999 page 15, subclause 18.2.3.3 */

      switch (rate)
        {
        case 2:
          return 10;
        case 4:
          return 20;
        case 11:
          return 55;
        case 22:
          return 110;

        /* IEEE Std 802.11g-2003 page 19, subclause 19.3.2.1 */

        case 44:
          return 220;
      }
    }
  else if (mode == IEEE80211_MODE_11G || mode == IEEE80211_MODE_11A)
    {
      /* IEEE Std 802.11a-1999 page 14, subclause 17.3.4.1 */

      switch (rate)
        {
        case 12:
          return 0x0b;
        case 18:
          return 0x0f;
        case 24:
          return 0x0a;
        case 36:
          return 0x0e;
        case 48:
          return 0x09;
        case 72:
          return 0x0d;
        case 96:
          return 0x08;
        case 108:
          return 0x0c;
        }
    }
  else
    {
      ndbg("ERROR: Unexpected mode %u", mode);
      PANIC();
    }

  ndbg("ERROR: unsupported rate %u\n", rate);
  return 0;
}

uint8_t ieee80211_plcp2rate(uint8_t plcp, enum ieee80211_phymode mode)
{
  if (mode == IEEE80211_MODE_11B)
    {
      /* IEEE Std 802.11g-2003 page 19, subclause 19.3.2.1 */

      switch (plcp)
        {
        case 10:
          return 2;
        case 20:
          return 4;
        case 55:
          return 11;
        case 110:
          return 22;

        /* IEEE Std 802.11g-2003 page 19, subclause 19.3.2.1 */

        case 220:
          return 44;
        }
    }
  else if (mode == IEEE80211_MODE_11G || mode == IEEE80211_MODE_11A)
    {
      /* IEEE Std 802.11a-1999 page 14, subclause 17.3.4.1 */

      switch (plcp)
        {
        case 0x0b:
          return 12;
        case 0x0f:
          return 18;
        case 0x0a:
          return 24;
        case 0x0e:
          return 36;
        case 0x09:
          return 48;
        case 0x0d:
          return 72;
        case 0x08:
          return 96;
        case 0x0c:
          return 108;
        }
    }
  else
    {
      ndbg("ERROR: Unexpected mode %u", mode);
      PANIC();
    }

  ndbg("ERROR: unsupported plcp %u\n", plcp);
  return 0;
}
