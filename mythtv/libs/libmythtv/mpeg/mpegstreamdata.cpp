// -*- Mode: c++ -*-
// Copyright (c) 2003-2004, Daniel Thor Kristjansson

#include <algorithm> // for find & max
using namespace std;

// POSIX headers
#include <sys/time.h> // for gettimeofday

// Qt headers
#include <QString>

// MythTV headers
#include "mpegstreamdata.h"
#include "mpegtables.h"
#include "ringbuffer.h"
#include "mpegtables.h"

#include "atscstreamdata.h"
#include "atsctables.h"

//#define DEBUG_MPEG_RADIO // uncomment to strip video streams from TS stream
#define LOC QString("MPEGStream[%1](0x%2): ").arg(m_cardId).arg((intptr_t)this, QT_POINTER_SIZE, 16, QChar('0'))

/** \class MPEGStreamData
 *  \brief Encapsulates data about MPEG stream and emits events for each table.
 */

/** \fn MPEGStreamData::MPEGStreamData(int, bool)
 *  \brief Initializes MPEGStreamData.
 *
 *   This adds the PID of the PAT table to "m_pidsListening"
 *
 *  \param desiredProgram If you want rewritten PAT and PMTs, for
 *                        a desired program set this to a value > -1
 *  \param cardnum        The card number that this stream is on.
 *                        Currently only used for differentiating streams
 *                        in log messages.
 *  \param cacheTables    If true PAT and PMT tables will be cached
 */
MPEGStreamData::MPEGStreamData(int desiredProgram, int cardnum,
                               bool cacheTables)
    : m_cardId(cardnum),
      m_cacheTables(cacheTables),
      // Single program stuff
      m_desiredProgram(desiredProgram)
{
    MPEGStreamData::AddListeningPID(MPEG_PAT_PID);
    MPEGStreamData::AddListeningPID(MPEG_CAT_PID);
}

MPEGStreamData::~MPEGStreamData()
{
    MPEGStreamData::Reset(-1);
    SetPATSingleProgram(nullptr);
    SetPMTSingleProgram(nullptr);

    // Delete any cached tables that haven't been returned
    psip_refcnt_map_t::iterator it = m_cachedSlatedForDeletion.begin();
    for (; it != m_cachedSlatedForDeletion.end(); ++it)
        delete it.key();

    QMutexLocker locker(&m_listenerLock);
    m_mpegListeners.clear();
    m_mpegSpListeners.clear();
}

void MPEGStreamData::SetDesiredProgram(int p)
{
    bool reset = true;
    uint pid = 0;
    const ProgramAssociationTable* pat = nullptr;
    pat_vec_t pats = GetCachedPATs();

    LOG(VB_RECORD, LOG_INFO, LOC + QString("SetDesiredProgram(%2)").arg(p));

    for (uint i = (p) ? 0 : pats.size(); i < pats.size() && !pid; i++)
    {
        pat = pats[i];
        pid = pats[i]->FindPID(p);
    }

    if (pid)
    {
        reset = false;
        m_desiredProgram = p;
        ProcessPAT(pat);
        pmt_vec_t pmts = GetCachedPMTs();
        for (auto & pmt : pmts)
        {
            if (pmt->ProgramNumber() == (uint)p)
                ProcessPMT(pmt);
        }
        ReturnCachedPMTTables(pmts);
    }

    ReturnCachedPATTables(pats);

    if (reset)
        Reset(p);
}

void MPEGStreamData::SetRecordingType(const QString &recording_type)
{
    m_recordingType = recording_type;
    uint neededAudio = (m_recordingType == "audio") ? 1 : 0;
    SetVideoStreamsRequired(0);
    SetAudioStreamsRequired(neededAudio);
}

void MPEGStreamData::SetEITHelper(EITHelper *eit_helper)
{
    QMutexLocker locker(&m_listenerLock);
    m_eitHelper = eit_helper;
}

void MPEGStreamData::SetEITRate(float rate)
{
    QMutexLocker locker(&m_listenerLock);
    m_eitRate = rate;
}

void MPEGStreamData::Reset(int desiredProgram)
{
    m_desiredProgram      = desiredProgram;
    m_recordingType       = "all";
    m_stripPmtDescriptors = false;
    m_normalizeStreamType = true;

    m_invalidPatSeen = false;

    SetPATSingleProgram(nullptr);
    SetPMTSingleProgram(nullptr);

    pid_psip_map_t old = m_partialPsipPacketCache;
    pid_psip_map_t::iterator it = old.begin();
    for (; it != old.end(); ++it)
        DeletePartialPSIP(it.key());
    m_partialPsipPacketCache.clear();

    m_pidsListening.clear();
    m_pidsNotListening.clear();
    m_pidsWriting.clear();
    m_pidsAudio.clear();

    m_pidVideoSingleProgram = m_pidPmtSingleProgram = 0xffffffff;

    m_patStatus.clear();

    m_pmtStatus.clear();

    {
        QMutexLocker locker(&m_cacheLock);

        pat_cache_t::iterator it1 = m_cachedPats.begin();
        for (; it1 != m_cachedPats.end(); ++it1)
            DeleteCachedTable(*it1);
        m_cachedPats.clear();

        pmt_cache_t::iterator it2 = m_cachedPmts.begin();
        for (; it2 != m_cachedPmts.end(); ++it2)
            DeleteCachedTable(*it2);
        m_cachedPmts.clear();

        cat_cache_t::iterator it3 = m_cachedCats.begin();
        for (; it3 != m_cachedCats.end(); ++it3)
            DeleteCachedTable(*it3);
        m_cachedCats.clear();
    }

    ResetDecryptionMonitoringState();

    AddListeningPID(MPEG_PAT_PID);
    AddListeningPID(MPEG_CAT_PID);
}

void MPEGStreamData::DeletePartialPSIP(uint pid)
{
    pid_psip_map_t::iterator it = m_partialPsipPacketCache.find(pid);
    if (it != m_partialPsipPacketCache.end())
    {
        PSIPTable *pkt = *it;
        m_partialPsipPacketCache.erase(it);
        delete pkt;
    }
}

/**
 *  \brief PSIP packet assembler.
 *
 *   This is not a general purpose TS->PSIP packet converter,
 *   it is only designed to work with MPEG tables which comply
 *   with certain restrictions that simplify the conversion.
 *
 *   DVB TSPackets may contain multiple segments of the PSI
 *   stream.  (see ISO 13818-1 section 2.4.3.3, particularly
 *   the definition of payload_unit_start_indicator, which
 *   indicates there is at least one segment start, but not
 *   limited to only one segment start.)
 *
 *   PSI stuffing bytes are 0xFF and will complete the
 *   remaining portion of the TSPacket.  (Section 2.4.4)
 *
 *  \note This method makes the assumption that AddTSPacket
 *        correctly handles duplicate packets.
 *
 *  \param tspacket Pointer the the TS packet data.
 *  \param moreTablePackets returns true if we need more packets
 */
PSIPTable* MPEGStreamData::AssemblePSIP(const TSPacket* tspacket,
                                        bool &moreTablePackets)
{
    bool broken = true;
    moreTablePackets = true;

    PSIPTable* partial = GetPartialPSIP(tspacket->PID());
    if (partial && partial->AddTSPacket(tspacket, broken) && !broken)
    {
        // check if it's safe to read pespacket's Length()
        if ((partial->PSIOffset() + 1 + 3) > partial->TSSizeInBuffer())
        {
            LOG(VB_RECORD, LOG_ERR, LOC +
                QString("Discarding broken PSIP packet. Packet's length at "
                        "position %1 isn't in the buffer of %2 bytes.")
                    .arg(partial->PSIOffset() + 1 + 3)
                    .arg(partial->TSSizeInBuffer()));
            DeletePartialPSIP(tspacket->PID());
            return nullptr;
        }

        // Discard broken packets
        bool buggy = m_haveCrcBug &&
        ((TableID::PMT == partial->StreamID()) ||
         (TableID::PAT == partial->StreamID()));
        if (!buggy && !partial->IsGood())
        {
            LOG(VB_SIPARSER, LOG_ERR, LOC + "Discarding broken PSIP packet");
            DeletePartialPSIP(tspacket->PID());
            return nullptr;
        }

        auto* psip = new PSIPTable(*partial);

        // Advance to the next packet
        // pesdata starts only at PSIOffset()+1
        uint packetStart = partial->PSIOffset() + 1 + psip->SectionLength();
        if (packetStart < partial->TSSizeInBuffer())
        {
            if (partial->pesdata()[psip->SectionLength()] != 0xff)
            {
#if 0 /* This doesn't work, you can't start PSIP packet like this
         because the PayloadStart() flag won't be set in this TSPacket
         -- dtk  May 4th, 2007
       */

                // If the next section starts in the new tspacket
                // create a new partial packet to prevent overflow
                if ((partial->TSSizeInBuffer() > TSPacket::kSize) &&
                    (packetStart >
                     partial->TSSizeInBuffer() - TSPacket::kPayloadSize))
                {
                    // Saving will handle deleting the old one
                    SavePartialPSIP(tspacket->PID(),
                                   new PSIPTable(*tspacket));
                }
                else
#endif
                {
                    partial->SetPSIOffset(partial->PSIOffset() +
                                          psip->SectionLength());
                }
                return psip;
            }
        }
        // discard incomplete packets
        if (packetStart > partial->TSSizeInBuffer())
        {
            LOG(VB_RECORD, LOG_ERR, LOC +
                QString("Discarding broken PSIP packet. ") +
                QString("Packet with %1 bytes doesn't fit "
                        "into a buffer of %2 bytes.")
                    .arg(packetStart).arg(partial->TSSizeInBuffer()));
            delete psip;
            psip = nullptr;
        }

        moreTablePackets = false;
        DeletePartialPSIP(tspacket->PID());
        return psip;
    }
    if (partial)
    {
        if (broken)
            DeletePartialPSIP(tspacket->PID());

        moreTablePackets = false;
        return nullptr; // partial packet is not yet complete.
    }

    if (!tspacket->PayloadStart())
    {
        // We didn't see this PSIP packet's start, so this must be the
        // tail end of something we missed. Ignore it.
        moreTablePackets = false;
        return nullptr;
    }

    // table_id (8 bits) and section_length(12), syntax(1), priv(1), res(2)
    // pointer_field (+8 bits), since payload start is true if we are here.
    const unsigned int extra_offset = 4;

    const unsigned int offset = tspacket->AFCOffset() + tspacket->StartOfFieldPointer();
    if (offset + extra_offset > TSPacket::kSize)
    {
        LOG(VB_GENERAL, LOG_ERR, LOC + QString("Error: "
                "AFCOffset(%1)+StartOfFieldPointer(%2)>184, "
                "pes length & current cannot be queried")
                .arg(tspacket->AFCOffset()).arg(tspacket->StartOfFieldPointer()));
        return nullptr;
    }

    const unsigned char* pesdata = tspacket->data() + offset;
    const unsigned int pes_length = (pesdata[2] & 0x0f) << 8 | pesdata[3];
    if ((pes_length + offset + extra_offset) > TSPacket::kSize)
    {
        SavePartialPSIP(tspacket->PID(), new PSIPTable(*tspacket));
        moreTablePackets = false;
        return nullptr;
    }

    auto *psip = new PSIPTable(*tspacket); // must be complete packet

    // There might be another section after this one in the
    // current packet. We need room before the end of the
    // packet, and it must not be packet stuffing.
    if ((offset + psip->SectionLength() < TSPacket::kSize) &&
        (pesdata[psip->SectionLength() + 1] != 0xff))
    {
        // This isn't stuffing, so we need to put this
        // on as a partial packet.
        auto *pesp = new PSIPTable(*tspacket);
        pesp->SetPSIOffset(offset + psip->SectionLength());
        SavePartialPSIP(tspacket->PID(), pesp);
        return psip;
    }

    moreTablePackets = false;
    return psip;
}

bool MPEGStreamData::CreatePATSingleProgram(
    const ProgramAssociationTable& pat)
{
    LOG(VB_RECORD, LOG_DEBUG, LOC + "CreatePATSingleProgram()");
    LOG(VB_RECORD, LOG_DEBUG, LOC + "PAT in input stream");
    LOG(VB_RECORD, LOG_DEBUG, LOC + pat.toString());
    if (m_desiredProgram < 0)
    {
        LOG(VB_RECORD, LOG_ERR, LOC + "Desired program not set yet");
        return false;
    }
    m_pidPmtSingleProgram = pat.FindPID(m_desiredProgram);
    LOG(VB_RECORD, LOG_DEBUG, LOC + QString("desired_program(%1) pid(0x%2)").
            arg(m_desiredProgram).arg(m_pidPmtSingleProgram, 0, 16));

    if (!m_pidPmtSingleProgram)
    {
        m_pidPmtSingleProgram = pat.FindAnyPID();
        if (!m_pidPmtSingleProgram)
        {
            LOG(VB_GENERAL, LOG_ERR, LOC + "No program found in PAT. "
                                     "This recording will not play in MythTV.");
        }
        LOG(VB_GENERAL, LOG_ERR, LOC +
            QString("Desired program #%1 not found in PAT."
                    "\n\t\t\tCannot create single program PAT.")
                .arg(m_desiredProgram));
        SetPATSingleProgram(nullptr);
        return false;
    }

    AddListeningPID(m_pidPmtSingleProgram);

    vector<uint> pnums;
    vector<uint> pids;

    pnums.push_back(1);
    pids.push_back(m_pidPmtSingleProgram);

    uint tsid = pat.TableIDExtension();
    uint ver = pat.Version();
    ProgramAssociationTable* pat2 =
        ProgramAssociationTable::Create(tsid, ver, pnums, pids);

    if (!pat2)
    {
        LOG(VB_GENERAL, LOG_ERR, LOC +
            "MPEGStreamData::CreatePATSingleProgram: "
            "Failed to create Program Association Table.");
        return false;
    }

    pat2->tsheader()->SetContinuityCounter(pat.tsheader()->ContinuityCounter());

    LOG(VB_RECORD, LOG_DEBUG, LOC + QString("pmt_pid(0x%1)")
            .arg(m_pidPmtSingleProgram, 0, 16));
    LOG(VB_RECORD, LOG_DEBUG, LOC + "PAT for output stream");
    LOG(VB_RECORD, LOG_DEBUG, LOC + pat2->toString());

    SetPATSingleProgram(pat2);

    return true;

}

static desc_list_t extract_atsc_desc(const tvct_vec_t &tvct,
                              const cvct_vec_t &cvct,
                              uint pnum)
{
    desc_list_t desc;

    vector<const VirtualChannelTable*> vct;

    for (auto i : tvct)
        vct.push_back(i);

    for (auto i : cvct)
        vct.push_back(i);

    for (size_t i = 0; i < tvct.size(); i++)
    {
        for (uint j = 0; j < vct[i]->ChannelCount(); j++)
        {
            if (vct[i]->ProgramNumber(j) == pnum)
            {
                desc_list_t ldesc = MPEGDescriptor::ParseOnlyInclude(
                    vct[i]->Descriptors(j), vct[i]->DescriptorsLength(j),
                    DescriptorID::caption_service);

                if (!ldesc.empty())
                    desc.insert(desc.end(), ldesc.begin(), ldesc.end());
            }
        }

        if (0 != vct[i]->GlobalDescriptorsLength())
        {
            desc_list_t vdesc = MPEGDescriptor::ParseOnlyInclude(
                vct[i]->GlobalDescriptors(),
                vct[i]->GlobalDescriptorsLength(),
                DescriptorID::caption_service);

            if (!vdesc.empty())
                desc.insert(desc.end(), vdesc.begin(), vdesc.end());
        }
    }

    return desc;
}

bool MPEGStreamData::CreatePMTSingleProgram(const ProgramMapTable &pmt)
{
    LOG(VB_RECORD, LOG_DEBUG, LOC + "CreatePMTSingleProgram()");
    LOG(VB_RECORD, LOG_DEBUG, LOC + "PMT in input stream");
    LOG(VB_RECORD, LOG_DEBUG, LOC + pmt.toString());

    if (!PATSingleProgram())
    {
        LOG(VB_RECORD, LOG_ERR, LOC + "no PAT yet...");
        return false; // no way to properly rewrite pids without PAT
    }
    pmt.Parse();

    uint programNumber = 1; // MPEG Program Number

    ATSCStreamData *sd = nullptr;
    tvct_vec_t tvct;
    cvct_vec_t cvct;

    desc_list_t gdesc;

    if (!m_stripPmtDescriptors)
    {
        gdesc = MPEGDescriptor::ParseAndExclude(
            pmt.ProgramInfo(), pmt.ProgramInfoLength(),
            DescriptorID::conditional_access);

        // If there is no caption descriptor in PMT, copy any caption
        // descriptor found in VCT to global descriptors...
        sd = dynamic_cast<ATSCStreamData*>(this);
        if (sd && !MPEGDescriptor::Find(gdesc, DescriptorID::caption_service))
        {
            tvct = sd->GetCachedTVCTs();
            cvct = sd->GetCachedCVCTs();

            desc_list_t vdesc = extract_atsc_desc(
                tvct, cvct, pmt.ProgramNumber());

            if (!vdesc.empty())
                gdesc.insert(gdesc.end(), vdesc.begin(), vdesc.end());
        }
    }

    vector<uint> pids;
    vector<uint> types;
    vector<desc_list_t> pdesc;

    uint video_cnt = 0;
    uint audio_cnt = 0;

    vector<uint> videoPIDs;
    vector<uint> audioPIDs;
    vector<uint> dataPIDs;

    for (uint i = 0; i < pmt.StreamCount(); i++)
    {
        uint pid = pmt.StreamPID(i);

        desc_list_t desc = MPEGDescriptor::ParseAndExclude(
            pmt.StreamInfo(i), pmt.StreamInfoLength(i),
            DescriptorID::conditional_access);

        uint type = StreamID::Normalize(
            pmt.StreamType(i), desc, m_siStandard);

        bool is_video = StreamID::IsVideo(type);
        bool is_audio = StreamID::IsAudio(type);

        if (is_audio)
        {
            audio_cnt++;
            audioPIDs.push_back(pid);
        }
        else if (m_recordingType == "audio" )
        {
            // If not an audio PID but we only want audio,
            // ignore this PID.
            continue;
        }

#ifdef DEBUG_MPEG_RADIO
        if (is_video)
            continue;
#endif // DEBUG_MPEG_RADIO

        if (is_video)
        {
            video_cnt++;
            videoPIDs.push_back(pid);
        }

        if (m_stripPmtDescriptors)
            desc.clear();

        // Filter out streams not used for basic television
        if (m_recordingType == "tv" && !is_audio && !is_video &&
            !MPEGDescriptor::Find(desc, DescriptorID::teletext) &&
            !MPEGDescriptor::Find(desc, DescriptorID::subtitling) &&
            pid != pmt.PCRPID()) // We must not strip the PCR!
        {
            continue;
        }

        if (!is_audio && !is_video) //NOTE: Anything which isn't audio or video is data
            dataPIDs.push_back(pid);

        pdesc.push_back(desc);
        pids.push_back(pid);
        types.push_back(type);
    }

    if (video_cnt < m_pmtSingleProgramNumVideo)
    {
        LOG(VB_RECORD, LOG_ERR, LOC +
            QString("Only %1 video streams seen in PMT, but %2 are required.")
                .arg(video_cnt).arg(m_pmtSingleProgramNumVideo));
        return false;
    }

    if (audioPIDs.size() < m_pmtSingleProgramNumAudio)
    {
        LOG(VB_RECORD, LOG_ERR, LOC +
            QString("Only %1 audio streams seen in PMT, but %2 are required.")
                .arg(audioPIDs.size()).arg(m_pmtSingleProgramNumAudio));
        return false;
    }

    desc_list_t cdesc = MPEGDescriptor::ParseOnlyInclude(
        pmt.ProgramInfo(), pmt.ProgramInfoLength(),
        DescriptorID::conditional_access);
    for (auto & i : cdesc)
    {
        ConditionalAccessDescriptor cad(i);
        if (cad.IsValid())
            AddListeningPID(cad.PID());
    }

    m_pidsAudio.clear();
    for (uint pid : audioPIDs)
        AddAudioPID(pid);

    m_pidsWriting.clear();
    m_pidVideoSingleProgram = !videoPIDs.empty() ? videoPIDs[0] : 0xffffffff;
    for (size_t i = 1; i < videoPIDs.size(); i++)
        AddWritingPID(videoPIDs[i]);

    for (uint pid : dataPIDs)
        AddWritingPID(pid);

    // Timebase
    int pcrpidIndex = pmt.FindPID(pmt.PCRPID());
    if (pcrpidIndex < 0)
    {
        // the timecode reference stream is not in the PMT,
        // add stream to misc record streams
        AddWritingPID(pmt.PCRPID());
    }

    // Create the PMT
    ProgramMapTable *pmt2 = ProgramMapTable::Create(
        programNumber, m_pidPmtSingleProgram, pmt.PCRPID(),
        pmt.Version(), gdesc, pids, types, pdesc);

    // Return any TVCT & CVCT tables, once we've copied any descriptors.
    if (sd)
    {
        sd->ReturnCachedTVCTTables(tvct);
        sd->ReturnCachedCVCTTables(cvct);
    }

    // Set Continuity Header
    uint cc_cnt = pmt.tsheader()->ContinuityCounter();
    pmt2->tsheader()->SetContinuityCounter(cc_cnt);
    SetPMTSingleProgram(pmt2);

    LOG(VB_RECORD, LOG_DEBUG, LOC + "PMT for output stream");
    LOG(VB_RECORD, LOG_DEBUG, LOC + pmt2->toString());

    return true;
}

/** \fn MPEGStreamData::IsRedundant(uint pid, const PSIPTable&) const
 *  \brief Returns true if table already seen.
 */
bool MPEGStreamData::IsRedundant(uint pid, const PSIPTable &psip) const
{
    (void) pid;
    const int table_id = psip.TableID();
    const int version  = psip.Version();

    if (TableID::PAT == table_id)
    {
        return m_patStatus.IsSectionSeen(psip.TableIDExtension(), version, psip.Section());
    }

    if (TableID::CAT == table_id)
    {
        return m_catStatus.IsSectionSeen(psip.TableIDExtension(), version, psip.Section());
    }

    if (TableID::PMT == table_id)
    {
        return m_pmtStatus.IsSectionSeen(psip.TableIDExtension(), version, psip.Section());
    }

    return false;
}

/** \fn MPEGStreamData::HandleTables(uint pid, const PSIPTable &psip)
 *  \brief Process PSIP packets.
 */
bool MPEGStreamData::HandleTables(uint pid, const PSIPTable &psip)
{
    if (MPEGStreamData::IsRedundant(pid, psip))
        return true;

    const int version = psip.Version();
    // If we get this far decode table
    switch (psip.TableID())
    {
        case TableID::PAT:
        {
            uint tsid = psip.TableIDExtension();
            m_patStatus.SetSectionSeen(tsid, version,  psip.Section(), psip.LastSection());

            ProgramAssociationTable pat(psip);

            if (m_cacheTables)
                CachePAT(&pat);

            ProcessPAT(&pat);

            return true;
        }
        case TableID::CAT:
        {
            uint tsid = psip.TableIDExtension();
            m_catStatus.SetSectionSeen(tsid, version, psip.Section(), psip.LastSection());

            ConditionalAccessTable cat(psip);

            if (m_cacheTables)
                CacheCAT(&cat);

            ProcessCAT(&cat);

            return true;
        }
        case TableID::PMT:
        {
            uint prog_num = psip.TableIDExtension();
            m_pmtStatus.SetSectionSeen(prog_num, version, psip.Section(), psip.LastSection());

            ProgramMapTable pmt(psip);

            if (m_cacheTables)
                CachePMT(&pmt);

            ProcessPMT(&pmt);

            return true;
        }
        case TableID::SITscte:
        {
            SpliceInformationTable sit(psip);
            sit.setSCTEPID(pid);

            m_listenerLock.lock();
            for (auto & listener : m_mpegListeners)
                listener->HandleSplice(&sit);
            m_listenerLock.unlock();

            return true;
        }
    }
    return false;
}

void MPEGStreamData::ProcessPAT(const ProgramAssociationTable *pat)
{
    bool foundProgram = pat->FindPID(m_desiredProgram) != 0U;

    m_listenerLock.lock();
    for (auto & listener : m_mpegListeners)
        listener->HandlePAT(pat);
    m_listenerLock.unlock();

    if (m_desiredProgram < 0)
        return;

    bool send_single_program = false;
    if (!m_invalidPatSeen && !foundProgram)
    {
        m_invalidPatSeen = true;
        m_invalidPatWarning = false;
        m_invalidPatTimer.start();
        LOG(VB_RECORD, LOG_WARNING, LOC +
            "ProcessPAT: PAT is missing program, setting timeout");
    }
    else if (m_invalidPatSeen && !foundProgram &&
             (m_invalidPatTimer.elapsed() > 400) && !m_invalidPatWarning)
    {
        m_invalidPatWarning = true; // only emit one warning...
        // After 400ms emit error if we haven't found correct PAT.
        LOG(VB_GENERAL, LOG_ERR, LOC + "ProcessPAT: Program not found in PAT. "
            "Rescan your transports.");

        send_single_program = CreatePATSingleProgram(*pat);
    }
    else if (foundProgram)
    {
        if (m_invalidPatSeen)
            LOG(VB_RECORD, LOG_INFO, LOC +
                "ProcessPAT: Good PAT seen after a bad PAT");

        m_invalidPatSeen = false;

        send_single_program = CreatePATSingleProgram(*pat);
    }

    if (send_single_program)
    {
        QMutexLocker locker(&m_listenerLock);
        ProgramAssociationTable *pat_sp = PATSingleProgram();
        for (auto & listener : m_mpegSpListeners)
            listener->HandleSingleProgramPAT(pat_sp, false);
    }
}

void MPEGStreamData::ProcessCAT(const ConditionalAccessTable *cat)
{
    m_listenerLock.lock();
    for (auto & listener : m_mpegListeners)
        listener->HandleCAT(cat);
    m_listenerLock.unlock();

    desc_list_t cdesc = MPEGDescriptor::ParseOnlyInclude(
        cat->Descriptors(), cat->DescriptorsLength(),
        DescriptorID::conditional_access);
    for (auto & i : cdesc)
    {
        ConditionalAccessDescriptor cad(i);
        if (cad.IsValid())
            AddListeningPID(cad.PID());
    }
}

void MPEGStreamData::ProcessPMT(const ProgramMapTable *pmt)
{
    m_listenerLock.lock();
    for (auto & listener : m_mpegListeners)
        listener->HandlePMT(pmt->ProgramNumber(), pmt);
    m_listenerLock.unlock();

    bool desired = pmt->ProgramNumber() == (uint) m_desiredProgram;
    if (desired && CreatePMTSingleProgram(*pmt))
    {
        QMutexLocker locker(&m_listenerLock);
        ProgramMapTable *pmt_sp = PMTSingleProgram();
        for (auto & listener : m_mpegSpListeners)
            listener->HandleSingleProgramPMT(pmt_sp, false);
    }
}

double MPEGStreamData::TimeOffset(void) const
{
    QMutexLocker locker(&m_siTimeLock);
    if (!m_siTimeOffsetCnt)
        return 0.0;

    double avg_offset = 0.0;
    double mult = 1.0 / m_siTimeOffsetCnt;
    for (uint i = 0; i < m_siTimeOffsetCnt; i++)
        avg_offset += m_siTimeOffsets[i] * mult;

    return avg_offset;
}

void MPEGStreamData::UpdateTimeOffset(uint64_t _si_utc_time)
{
    struct timeval tm {};
    if (gettimeofday(&tm, nullptr) != 0)
        return;

    double utc_time = tm.tv_sec + (tm.tv_usec * 0.000001);
    double si_time  = _si_utc_time;

    QMutexLocker locker(&m_siTimeLock);
    m_siTimeOffsets[m_siTimeOffsetIndx] = si_time - utc_time;

    if (m_siTimeOffsetIndx + 1 > m_siTimeOffsetCnt)
        m_siTimeOffsetCnt = m_siTimeOffsetIndx + 1;

    m_siTimeOffsetIndx = (m_siTimeOffsetIndx + 1) & 0xf;

}

#define DONE_WITH_PSIP_PACKET() { delete psip; \
    if (morePSIPTables) goto HAS_ANOTHER_PSIP; else return; }

/** \fn MPEGStreamData::HandleTSTables(const TSPacket*)
 *  \brief Assembles PSIP packets and processes them.
 */
void MPEGStreamData::HandleTSTables(const TSPacket* tspacket)
{
    bool morePSIPTables = false;
  HAS_ANOTHER_PSIP:
    // Assemble PSIP
    PSIPTable *psip = AssemblePSIP(tspacket, morePSIPTables);
    if (!psip)
       return;

    // drop stuffing packets
    if ((TableID::ST       == psip->TableID()) ||
        (TableID::STUFFING == psip->TableID()))
    {
        LOG(VB_RECORD, LOG_DEBUG, LOC + "Dropping Stuffing table");
        DONE_WITH_PSIP_PACKET();
    }

    // Don't do validation on tables without CRC
    if (!psip->HasCRC())
    {
        HandleTables(tspacket->PID(), *psip);
        DONE_WITH_PSIP_PACKET();
    }

    // Validate PSIP
    // but don't validate PMT/PAT if our driver has the PMT/PAT CRC bug.
    bool buggy = m_haveCrcBug &&
        ((TableID::PMT == psip->TableID()) ||
         (TableID::PAT == psip->TableID()));
    if (!buggy && !psip->IsGood())
    {
        LOG(VB_RECORD, LOG_ERR, LOC +
            QString("PSIP packet failed CRC check. pid(0x%1) type(0x%2)")
                .arg(tspacket->PID(),0,16).arg(psip->TableID(),0,16));
        DONE_WITH_PSIP_PACKET();
    }

    if (TableID::MGT <= psip->TableID() && psip->TableID() <= TableID::STT &&
        !psip->IsCurrent())
    { // we don't cache the next table, for now
        LOG(VB_RECORD, LOG_DEBUG, LOC + QString("Table not current 0x%1")
            .arg(psip->TableID(),2,16,QChar('0')));
        DONE_WITH_PSIP_PACKET();
    }

    if (tspacket->Scrambled())
    { // scrambled! ATSC, DVB require tables not to be scrambled
        LOG(VB_RECORD, LOG_ERR, LOC +
            "PSIP packet is scrambled, not ATSC/DVB compiant");
        DONE_WITH_PSIP_PACKET();
    }

    if (!psip->VerifyPSIP(!m_haveCrcBug))
    {
        LOG(VB_RECORD, LOG_ERR, LOC + QString("PSIP table 0x%1 is invalid")
            .arg(psip->TableID(),2,16,QChar('0')));
        DONE_WITH_PSIP_PACKET();
    }

    // Don't decode redundant packets,
    // but if it is a desired PAT or PMT emit a "heartbeat" signal.
    if (MPEGStreamData::IsRedundant(tspacket->PID(), *psip))
    {
        if (TableID::PAT == psip->TableID())
        {
            QMutexLocker locker(&m_listenerLock);
            ProgramAssociationTable *pat_sp = PATSingleProgram();
            for (auto & listener : m_mpegSpListeners)
                listener->HandleSingleProgramPAT(pat_sp, false);
        }
        if (TableID::PMT == psip->TableID() &&
            tspacket->PID() == m_pidPmtSingleProgram)
        {
            QMutexLocker locker(&m_listenerLock);
            ProgramMapTable *pmt_sp = PMTSingleProgram();
            for (auto & listener : m_mpegSpListeners)
                listener->HandleSingleProgramPMT(pmt_sp, false);
        }
        DONE_WITH_PSIP_PACKET(); // already parsed this table, toss it.
    }

    HandleTables(tspacket->PID(), *psip);

    DONE_WITH_PSIP_PACKET();
}
#undef DONE_WITH_PSIP_PACKET

int MPEGStreamData::ProcessData(const unsigned char *buffer, int len)
{
    int pos = 0;
    bool resync = false;

    if (!m_psListeners.empty())
    {

        for (auto & listener : m_psListeners)
            listener->FindPSKeyFrames(buffer, len);

        return 0;
    }

    while (pos + int(TSPacket::kSize) <= len)
    { // while we have a whole packet left...
        if (buffer[pos] != SYNC_BYTE || resync)
        {
            int newpos = ResyncStream(buffer, pos+1, len);
            LOG(VB_RECORD, LOG_DEBUG, LOC +
                QString("Resyncing @ %1+1 w/len %2 -> %3")
                .arg(pos).arg(len).arg(newpos));
            if (newpos == -1)
                return len - pos;
            if (newpos == -2)
                return TSPacket::kSize;
            pos = newpos;
        }

        const auto *pkt = reinterpret_cast<const TSPacket*>(&buffer[pos]);
        pos += TSPacket::kSize; // Advance to next TS packet
        resync = false;
        if (!ProcessTSPacket(*pkt))
        {
            if (pos + int(TSPacket::kSize) > len)
                continue;
            if (buffer[pos] != SYNC_BYTE)
            {
                // if ProcessTSPacket fails, and we don't appear to be
                // in sync on the next packet, then resync. Otherwise
                // just process the next packet normally.
                pos -= TSPacket::kSize;
                resync = true;
            }
        }
    }

    return len - pos;
}

bool MPEGStreamData::ProcessTSPacket(const TSPacket& tspacket)
{
    bool ok = !tspacket.TransportError();

    if (IsEncryptionTestPID(tspacket.PID()))
    {
        ProcessEncryptedPacket(tspacket);
    }

    if (!ok)
        return false;

    if (tspacket.Scrambled())
        return true;

    if (VERBOSE_LEVEL_CHECK(VB_RECORD, LOG_DEBUG))
    {
        if (m_pmtSingleProgram && tspacket.PID() ==
            m_pmtSingleProgram->PCRPID())
        {
            if (tspacket.HasPCR())
            {
                LOG(VB_RECORD, LOG_DEBUG, LOC +
                    QString("PID %1 (0x%2) has PCR %3μs")
                    .arg(m_pmtSingleProgram->PCRPID())
                    .arg(m_pmtSingleProgram->PCRPID(), 0, 16)
                    .arg(std::chrono::duration_cast<std::chrono::microseconds>
                         (tspacket.GetPCR().time_since_epoch()).count()));
            }
        }
    }

    if (IsVideoPID(tspacket.PID()))
    {
        for (auto & listener : m_tsAvListeners)
            listener->ProcessVideoTSPacket(tspacket);

        return true;
    }

    if (IsAudioPID(tspacket.PID()))
    {
        for (auto & listener : m_tsAvListeners)
            listener->ProcessAudioTSPacket(tspacket);

        return true;
    }

    if (IsWritingPID(tspacket.PID()))
    {
        for (auto & listener : m_tsWritingListeners)
            listener->ProcessTSPacket(tspacket);
    }

    if (IsListeningPID(tspacket.PID()) && tspacket.HasPayload())
    {
        HandleTSTables(&tspacket);
    }

    return true;
}

int MPEGStreamData::ResyncStream(const unsigned char *buffer, int curr_pos,
                                 int len)
{
    // Search for two sync bytes 188 bytes apart,
    int pos = curr_pos;
    int nextpos = pos + TSPacket::kSize;
    if (nextpos >= len)
        return -1; // not enough bytes; caller should try again

    while (buffer[pos] != SYNC_BYTE || buffer[nextpos] != SYNC_BYTE)
    {
        pos++;
        nextpos++;
        if (nextpos == len)
            return -2; // not found
    }

    return pos;
}

bool MPEGStreamData::IsListeningPID(uint pid) const
{
    if (m_listeningDisabled || IsNotListeningPID(pid))
        return false;
    pid_map_t::const_iterator it = m_pidsListening.find(pid);
    return it != m_pidsListening.end();
}

bool MPEGStreamData::IsNotListeningPID(uint pid) const
{
    pid_map_t::const_iterator it = m_pidsNotListening.find(pid);
    return it != m_pidsNotListening.end();
}

bool MPEGStreamData::IsWritingPID(uint pid) const
{
    pid_map_t::const_iterator it = m_pidsWriting.find(pid);
    return it != m_pidsWriting.end();
}

bool MPEGStreamData::IsAudioPID(uint pid) const
{
    pid_map_t::const_iterator it = m_pidsAudio.find(pid);
    return it != m_pidsAudio.end();
}

uint MPEGStreamData::GetPIDs(pid_map_t &pids) const
{
    uint sz = pids.size();

    if (m_pidVideoSingleProgram < 0x1fff)
        pids[m_pidVideoSingleProgram] = kPIDPriorityHigh;

    pid_map_t::const_iterator it = m_pidsListening.begin();
    for (; it != m_pidsListening.end(); ++it)
        pids[it.key()] = max(pids[it.key()], *it);

    it = m_pidsAudio.begin();
    for (; it != m_pidsAudio.end(); ++it)
        pids[it.key()] = max(pids[it.key()], *it);

    it = m_pidsWriting.begin();
    for (; it != m_pidsWriting.end(); ++it)
        pids[it.key()] = max(pids[it.key()], *it);

    return pids.size() - sz;
}

PIDPriority MPEGStreamData::GetPIDPriority(uint pid) const
{
    if (m_pidVideoSingleProgram == pid)
        return kPIDPriorityHigh;

    pid_map_t::const_iterator it;
    it = m_pidsListening.find(pid);
    if (it != m_pidsListening.end())
        return *it;
    it = m_pidsNotListening.find(pid);
    if (it != m_pidsNotListening.end())
        return *it;
    it = m_pidsWriting.find(pid);
    if (it != m_pidsWriting.end())
        return *it;
    it = m_pidsAudio.find(pid);
    if (it != m_pidsAudio.end())
        return *it;

    return kPIDPriorityNone;
}

void MPEGStreamData::SavePartialPSIP(uint pid, PSIPTable* packet)
{
    pid_psip_map_t::iterator it = m_partialPsipPacketCache.find(pid);
    if (it == m_partialPsipPacketCache.end())
        m_partialPsipPacketCache[pid] = packet;
    else
    {
        PSIPTable *old = *it;
        m_partialPsipPacketCache.remove(pid);
        m_partialPsipPacketCache.insert(pid, packet);
        delete old;
    }
}

bool MPEGStreamData::HasAllPATSections(uint tsid) const
{
    return m_patStatus.HasAllSections(tsid);
}

bool MPEGStreamData::HasAllCATSections(uint tsid) const
{
    return m_catStatus.HasAllSections(tsid);
}

bool MPEGStreamData::HasAllPMTSections(uint prog_num) const
{
    return m_pmtStatus.HasAllSections(prog_num);
}

bool MPEGStreamData::HasProgram(uint progNum) const
{
    pmt_const_ptr_t pmt = GetCachedPMT(progNum, 0);
    bool hasit = pmt;
    ReturnCachedTable(pmt);

    return hasit;
}

bool MPEGStreamData::HasCachedAllPAT(uint tsid) const
{
    QMutexLocker locker(&m_cacheLock);

    pat_cache_t::const_iterator it = m_cachedPats.find(tsid << 8);
    if (it == m_cachedPats.end())
        return false;

    uint last_section = (*it)->LastSection();
    if (!last_section)
        return true;

    for (uint i = 1; i <= last_section; i++)
        if (m_cachedPats.find((tsid << 8) | i) == m_cachedPats.end())
            return false;

    return true;
}

bool MPEGStreamData::HasCachedAnyPAT(uint tsid) const
{
    QMutexLocker locker(&m_cacheLock);

    for (uint i = 0; i <= 255; i++)
        if (m_cachedPats.find((tsid << 8) | i) != m_cachedPats.end())
            return true;

    return false;
}

bool MPEGStreamData::HasCachedAnyPAT(void) const
{
    QMutexLocker locker(&m_cacheLock);
    return !m_cachedPats.empty();
}

bool MPEGStreamData::HasCachedAllCAT(uint tsid) const
{
    QMutexLocker locker(&m_cacheLock);

    cat_cache_t::const_iterator it = m_cachedCats.find(tsid << 8);
    if (it == m_cachedCats.end())
        return false;

    uint last_section = (*it)->LastSection();
    if (!last_section)
        return true;

    for (uint i = 1; i <= last_section; i++)
        if (m_cachedCats.find((tsid << 8) | i) == m_cachedCats.end())
            return false;

    return true;
}

bool MPEGStreamData::HasCachedAnyCAT(uint tsid) const
{
    QMutexLocker locker(&m_cacheLock);

    for (uint i = 0; i <= 255; i++)
        if (m_cachedCats.find((tsid << 8) | i) != m_cachedCats.end())
            return true;

    return false;
}

bool MPEGStreamData::HasCachedAnyCAT(void) const
{
    QMutexLocker locker(&m_cacheLock);
    return !m_cachedCats.empty();
}

bool MPEGStreamData::HasCachedAllPMT(uint pnum) const
{
    QMutexLocker locker(&m_cacheLock);

    pmt_cache_t::const_iterator it = m_cachedPmts.find(pnum << 8);
    if (it == m_cachedPmts.end())
        return false;

    uint last_section = (*it)->LastSection();
    if (!last_section)
        return true;

    for (uint i = 1; i <= last_section; i++)
        if (m_cachedPmts.find((pnum << 8) | i) == m_cachedPmts.end())
            return false;

    return true;
}

bool MPEGStreamData::HasCachedAnyPMT(uint pnum) const
{
    QMutexLocker locker(&m_cacheLock);

    for (uint i = 0; i <= 255; i++)
        if (m_cachedPmts.find((pnum << 8) | i) != m_cachedPmts.end())
            return true;

    return false;
}

bool MPEGStreamData::HasCachedAllPMTs(void) const
{
    QMutexLocker locker(&m_cacheLock);

    if (m_cachedPats.empty())
        return false;

    pat_cache_t::const_iterator it = m_cachedPats.begin();
    for (; it != m_cachedPats.end(); ++it)
    {
        const ProgramAssociationTable *pat = *it;
        if (!HasCachedAllPAT(pat->TransportStreamID()))
            return false;

        for (uint i = 0; i < pat->ProgramCount(); i++)
        {
            uint prognum = pat->ProgramNumber(i);
            if (prognum && !HasCachedAllPMT(prognum))
                return false;
        }
    }

    return true;
}

bool MPEGStreamData::HasCachedAnyPMTs(void) const
{
    QMutexLocker locker(&m_cacheLock);
    return !m_cachedPmts.empty();
}

pat_const_ptr_t MPEGStreamData::GetCachedPAT(uint tsid, uint section_num) const
{
    QMutexLocker locker(&m_cacheLock);
    ProgramAssociationTable *pat = nullptr;

    uint key = (tsid << 8) | section_num;
    pat_cache_t::const_iterator it = m_cachedPats.find(key);
    if (it != m_cachedPats.end())
        IncrementRefCnt(pat = *it);

    return pat;
}

pat_vec_t MPEGStreamData::GetCachedPATs(uint tsid) const
{
    QMutexLocker locker(&m_cacheLock);
    pat_vec_t pats;

    for (uint i=0; i < 256; i++)
    {
        pat_const_ptr_t pat = GetCachedPAT(tsid, i);
        if (pat)
            pats.push_back(pat);
    }

    return pats;
}

pat_vec_t MPEGStreamData::GetCachedPATs(void) const
{
    QMutexLocker locker(&m_cacheLock);
    pat_vec_t pats;

    pat_cache_t::const_iterator it = m_cachedPats.begin();
    for (; it != m_cachedPats.end(); ++it)
    {
        ProgramAssociationTable* pat = *it;
        IncrementRefCnt(pat);
        pats.push_back(pat);
    }

    return pats;
}

cat_const_ptr_t MPEGStreamData::GetCachedCAT(uint tsid, uint section_num) const
{
    QMutexLocker locker(&m_cacheLock);
    ConditionalAccessTable *cat = nullptr;

    uint key = (tsid << 8) | section_num;
    cat_cache_t::const_iterator it = m_cachedCats.find(key);
    if (it != m_cachedCats.end())
        IncrementRefCnt(cat = *it);

    return cat;
}

cat_vec_t MPEGStreamData::GetCachedCATs(uint tsid) const
{
    QMutexLocker locker(&m_cacheLock);
    cat_vec_t cats;

    for (uint i=0; i < 256; i++)
    {
        cat_const_ptr_t cat = GetCachedCAT(tsid, i);
        if (cat)
            cats.push_back(cat);
    }

    return cats;
}

cat_vec_t MPEGStreamData::GetCachedCATs(void) const
{
    QMutexLocker locker(&m_cacheLock);
    cat_vec_t cats;

    cat_cache_t::const_iterator it = m_cachedCats.begin();
    for (; it != m_cachedCats.end(); ++it)
    {
        ConditionalAccessTable* cat = *it;
        IncrementRefCnt(cat);
        cats.push_back(cat);
    }

    return cats;
}

pmt_const_ptr_t MPEGStreamData::GetCachedPMT(
    uint program_num, uint section_num) const
{
    QMutexLocker locker(&m_cacheLock);
    ProgramMapTable *pmt = nullptr;

    uint key = (program_num << 8) | section_num;
    pmt_cache_t::const_iterator it = m_cachedPmts.find(key);
    if (it != m_cachedPmts.end())
        IncrementRefCnt(pmt = *it);

    return pmt;
}

pmt_vec_t MPEGStreamData::GetCachedPMTs(void) const
{
    QMutexLocker locker(&m_cacheLock);
    vector<const ProgramMapTable*> pmts;

    pmt_cache_t::const_iterator it = m_cachedPmts.begin();
    for (; it != m_cachedPmts.end(); ++it)
    {
        ProgramMapTable* pmt = *it;
        IncrementRefCnt(pmt);
        pmts.push_back(pmt);
    }

    return pmts;
}

pmt_map_t MPEGStreamData::GetCachedPMTMap(void) const
{
    QMutexLocker locker(&m_cacheLock);
    pmt_map_t pmts;

    pmt_cache_t::const_iterator it = m_cachedPmts.begin();
    for (; it != m_cachedPmts.end(); ++it)
    {
        ProgramMapTable* pmt = *it;
        IncrementRefCnt(pmt);
        pmts[pmt->ProgramNumber()].push_back(pmt);
    }

    return pmts;
}

void MPEGStreamData::ReturnCachedTable(const PSIPTable *psip) const
{
    QMutexLocker locker(&m_cacheLock);

    int val = m_cachedRefCnt[psip] - 1;
    m_cachedRefCnt[psip] = val;

    // if ref <= 0 and table was slated for deletion, delete it.
    if (val <= 0)
    {
        psip_refcnt_map_t::iterator it;
        it = m_cachedSlatedForDeletion.find(psip);
        if (it != m_cachedSlatedForDeletion.end())
            DeleteCachedTable(psip);
    }
}

void MPEGStreamData::ReturnCachedPATTables(pat_vec_t &pats) const
{
    for (auto & pat : pats)
        ReturnCachedTable(pat);
    pats.clear();
}

void MPEGStreamData::ReturnCachedPATTables(pat_map_t &pats) const
{
    for (pat_map_t::iterator it = pats.begin(); it != pats.end(); ++it)
        ReturnCachedPATTables(*it);
    pats.clear();
}

void MPEGStreamData::ReturnCachedCATTables(cat_vec_t &cats) const
{
    for (auto & cat : cats)
        ReturnCachedTable(cat);
    cats.clear();
}

void MPEGStreamData::ReturnCachedCATTables(cat_map_t &cats) const
{
    for (cat_map_t::iterator it = cats.begin(); it != cats.end(); ++it)
        ReturnCachedCATTables(*it);
    cats.clear();
}

void MPEGStreamData::ReturnCachedPMTTables(pmt_vec_t &pmts) const
{
    for (auto & pmt : pmts)
        ReturnCachedTable(pmt);
    pmts.clear();
}

void MPEGStreamData::ReturnCachedPMTTables(pmt_map_t &pmts) const
{
    for (pmt_map_t::iterator it = pmts.begin(); it != pmts.end(); ++it)
        ReturnCachedPMTTables(*it);
    pmts.clear();
}

void MPEGStreamData::IncrementRefCnt(const PSIPTable *psip) const
{
    QMutexLocker locker(&m_cacheLock);
    m_cachedRefCnt[psip] = m_cachedRefCnt[psip] + 1;
}

bool MPEGStreamData::DeleteCachedTable(const PSIPTable *psip) const
{
    if (!psip)
        return false;

    uint tid = psip->TableIDExtension();

    QMutexLocker locker(&m_cacheLock);
    if (m_cachedRefCnt[psip] > 0)
    {
        m_cachedSlatedForDeletion[psip] = 1;
        return false;
    }
    if (TableID::PAT == psip->TableID() &&
             (m_cachedPats[(tid << 8) | psip->Section()] == psip))
    {
        m_cachedPats[(tid << 8) | psip->Section()] = nullptr;
        delete psip;
    }
    else if (TableID::CAT == psip->TableID() &&
             (m_cachedCats[(tid << 8) | psip->Section()] == psip))
    {
        m_cachedCats[(tid << 8) | psip->Section()] = nullptr;
        delete psip;
    }
    else if ((TableID::PMT == psip->TableID()) &&
             (m_cachedPmts[(tid << 8) | psip->Section()] == psip))
    {
        m_cachedPmts[(tid << 8) | psip->Section()] = nullptr;
        delete psip;
    }
    else
    {
        m_cachedSlatedForDeletion[psip] = 2;
        return false;
    }
    psip_refcnt_map_t::iterator it;
    it = m_cachedSlatedForDeletion.find(psip);
    if (it != m_cachedSlatedForDeletion.end())
        m_cachedSlatedForDeletion.erase(it);

    return true;
}

void MPEGStreamData::CachePAT(const ProgramAssociationTable *_pat)
{
    auto *pat = new ProgramAssociationTable(*_pat);
    uint key = (_pat->TransportStreamID() << 8) | _pat->Section();

    QMutexLocker locker(&m_cacheLock);

    pat_cache_t::iterator it = m_cachedPats.find(key);
    if (it != m_cachedPats.end())
        DeleteCachedTable(*it);

    m_cachedPats[key] = pat;
}

void MPEGStreamData::CacheCAT(const ConditionalAccessTable *_cat)
{
    auto *cat = new ConditionalAccessTable(*_cat);
    uint key = (_cat->TableIDExtension() << 8) | _cat->Section();

    QMutexLocker locker(&m_cacheLock);

    cat_cache_t::iterator it = m_cachedCats.find(key);
    if (it != m_cachedCats.end())
        DeleteCachedTable(*it);

    m_cachedCats[key] = cat;
}

void MPEGStreamData::CachePMT(const ProgramMapTable *_pmt)
{
    auto *pmt = new ProgramMapTable(*_pmt);
    uint key = (_pmt->ProgramNumber() << 8) | _pmt->Section();

    QMutexLocker locker(&m_cacheLock);

    pmt_cache_t::iterator it = m_cachedPmts.find(key);
    if (it != m_cachedPmts.end())
        DeleteCachedTable(*it);

    m_cachedPmts[key] = pmt;
}

void MPEGStreamData::AddMPEGListener(MPEGStreamListener *val)
{
    QMutexLocker locker(&m_listenerLock);

    auto it = m_mpegListeners.begin();
    for (; it != m_mpegListeners.end(); ++it)
        if (((void*)val) == ((void*)*it))
            return;

    m_mpegListeners.push_back(val);
}

void MPEGStreamData::RemoveMPEGListener(MPEGStreamListener *val)
{
    QMutexLocker locker(&m_listenerLock);

    auto it = m_mpegListeners.begin();
    for (; it != m_mpegListeners.end(); ++it)
    {
        if (((void*)val) == ((void*)*it))
        {
            m_mpegListeners.erase(it);
            return;
        }
    }
}

void MPEGStreamData::AddWritingListener(TSPacketListener *val)
{
    QMutexLocker locker(&m_listenerLock);

    auto it = m_tsWritingListeners.begin();
    for (; it != m_tsWritingListeners.end(); ++it)
        if (((void*)val) == ((void*)*it))
            return;

    m_tsWritingListeners.push_back(val);
}

void MPEGStreamData::RemoveWritingListener(TSPacketListener *val)
{
    QMutexLocker locker(&m_listenerLock);

    auto it = m_tsWritingListeners.begin();
    for (; it != m_tsWritingListeners.end(); ++it)
    {
        if (((void*)val) == ((void*)*it))
        {
            m_tsWritingListeners.erase(it);
            return;
        }
    }
}

void MPEGStreamData::AddAVListener(TSPacketListenerAV *val)
{
    QMutexLocker locker(&m_listenerLock);

    auto it = m_tsAvListeners.begin();
    for (; it != m_tsAvListeners.end(); ++it)
        if (((void*)val) == ((void*)*it))
            return;

    m_tsAvListeners.push_back(val);
}

void MPEGStreamData::RemoveAVListener(TSPacketListenerAV *val)
{
    QMutexLocker locker(&m_listenerLock);

    auto it = m_tsAvListeners.begin();
    for (; it != m_tsAvListeners.end(); ++it)
    {
        if (((void*)val) == ((void*)*it))
        {
            m_tsAvListeners.erase(it);
            return;
        }
    }
}

void MPEGStreamData::AddMPEGSPListener(MPEGSingleProgramStreamListener *val)
{
    QMutexLocker locker(&m_listenerLock);

    auto it = m_mpegSpListeners.begin();
    for (; it != m_mpegSpListeners.end(); ++it)
        if (((void*)val) == ((void*)*it))
            return;

    m_mpegSpListeners.push_back(val);
}

void MPEGStreamData::RemoveMPEGSPListener(MPEGSingleProgramStreamListener *val)
{
    QMutexLocker locker(&m_listenerLock);

    auto it = m_mpegSpListeners.begin();
    for (; it != m_mpegSpListeners.end(); ++it)
    {
        if (((void*)val) == ((void*)*it))
        {
            m_mpegSpListeners.erase(it);
            return;
        }
    }
}

void MPEGStreamData::AddPSStreamListener(PSStreamListener *val)
{
    QMutexLocker locker(&m_listenerLock);

    auto it = m_psListeners.begin();
    for (; it != m_psListeners.end(); ++it)
        if (((void*)val) == ((void*)*it))
            return;

    m_psListeners.push_back(val);
}

void MPEGStreamData::RemovePSStreamListener(PSStreamListener *val)
{
    QMutexLocker locker(&m_listenerLock);

    auto it = m_psListeners.begin();
    for (; it != m_psListeners.end(); ++it)
    {
        if (((void*)val) == ((void*)*it))
        {
            m_psListeners.erase(it);
            return;
        }
    }
}

void MPEGStreamData::AddEncryptionTestPID(uint pnum, uint pid, bool isvideo)
{
    QMutexLocker locker(&m_encryptionLock);

#if 0
    LOG(VB_GENERAL, LOG_DEBUG, LOC + QString("AddEncryptionTestPID(%1, 0x%2)")
            .arg(pnum) .arg(pid, 0, 16));
#endif

    AddListeningPID(pid);

    m_encryptionPidToInfo[pid] = CryptInfo((isvideo) ? 10000 : 500, 8);

    m_encryptionPidToPnums[pid].push_back(pnum);
    m_encryptionPnumToPids[pnum].push_back(pid);
    m_encryptionPnumToStatus[pnum] = kEncUnknown;
}

void MPEGStreamData::RemoveEncryptionTestPIDs(uint pnum)
{
    QMutexLocker locker(&m_encryptionLock);

#if 0
    LOG(VB_RECORD, LOG_DEBUG, LOC +
        QString("Tearing down up decryption monitoring for program %1")
            .arg(pnum));
#endif

    QMap<uint, uint_vec_t>::iterator list;
    uint_vec_t::iterator it;

    uint_vec_t pids = m_encryptionPnumToPids[pnum];
    for (uint pid : pids)
    {
#if 0
        LOG(VB_GENERAL, LOG_DEBUG, LOC +
            QString("Removing 0x%1 PID Enc monitoring").arg(pid,0,16));
#endif

        RemoveListeningPID(pid);

        list = m_encryptionPidToPnums.find(pid);
        if (list != m_encryptionPidToPnums.end())
        {
            it = find((*list).begin(), (*list).end(), pnum);

            if (it != (*list).end())
                (*list).erase(it);

            if ((*list).empty())
            {
                m_encryptionPidToPnums.remove(pid);
                m_encryptionPidToInfo.remove(pid);
            }
        }
    }

    m_encryptionPnumToPids.remove(pnum);
}

bool MPEGStreamData::IsEncryptionTestPID(uint pid) const
{
    QMutexLocker locker(&m_encryptionLock);

    QMap<uint, CryptInfo>::const_iterator it =
        m_encryptionPidToInfo.find(pid);

    return it != m_encryptionPidToInfo.end();
}

void MPEGStreamData::TestDecryption(const ProgramMapTable *pmt)
{
    QMutexLocker locker(&m_encryptionLock);

#if 0
    LOG(VB_RECORD, LOG_DEBUG, LOC +
        QString("Setting up decryption monitoring for program %1")
            .arg(pmt->ProgramNumber()));
#endif

    bool encrypted = pmt->IsProgramEncrypted();
    for (uint i = 0; i < pmt->StreamCount(); i++)
    {
        if (!encrypted && !pmt->IsStreamEncrypted(i))
            continue;

        bool is_vid = pmt->IsVideo(i, m_siStandard);
        bool is_aud = pmt->IsAudio(i, m_siStandard);
        if (is_vid || is_aud)
        {
            AddEncryptionTestPID(
                pmt->ProgramNumber(), pmt->StreamPID(i), is_vid);
        }
    }
}

void MPEGStreamData::ResetDecryptionMonitoringState(void)
{
    QMutexLocker locker(&m_encryptionLock);

    m_encryptionPidToInfo.clear();
    m_encryptionPidToPnums.clear();
    m_encryptionPnumToPids.clear();
}

bool MPEGStreamData::IsProgramDecrypted(uint pnum) const
{
    QMutexLocker locker(&m_encryptionLock);
    return m_encryptionPnumToStatus[pnum] == kEncDecrypted;
}

bool MPEGStreamData::IsProgramEncrypted(uint pnum) const
{
    QMutexLocker locker(&m_encryptionLock);
    return m_encryptionPnumToStatus[pnum] == kEncEncrypted;
}

static QString toString(CryptStatus status)
{
    if (kEncDecrypted == status)
        return "Decrypted";
    if (kEncEncrypted == status)
        return "Encrypted";
    return "Unknown";
}

/** \fn MPEGStreamData::ProcessEncryptedPacket(const TSPacket& tspacket)
 *  \brief counts en/decrypted packets to decide if a stream is en/decrypted
 */
void MPEGStreamData::ProcessEncryptedPacket(const TSPacket& tspacket)
{
    QMutexLocker locker(&m_encryptionLock);

    const uint pid = tspacket.PID();
    CryptInfo &info = m_encryptionPidToInfo[pid];

    CryptStatus status = kEncUnknown;

    if (tspacket.Scrambled())
    {
        info.m_decryptedPackets = 0;

        // If a fair amount of encrypted packets is passed assume that
        // the stream is not decryptable
        if (++info.m_encryptedPackets >= info.m_encryptedMin)
            status = kEncEncrypted;
    }
    else
    {
        info.m_encryptedPackets = 0;
        if (++info.m_decryptedPackets > info.m_decryptedMin)
            status = kEncDecrypted;
    }

    if (status == info.m_status)
        return; // pid encryption status unchanged

    info.m_status = status;

    LOG(status != kEncDecrypted ? VB_GENERAL : VB_RECORD, LOG_DEBUG, LOC +
        QString("PID 0x%1 status: %2") .arg(pid,0,16).arg(toString(status)));

    uint_vec_t pnum_del_list;
    const uint_vec_t &pnums = m_encryptionPidToPnums[pid];
    for (uint pnum : pnums)
    {
        status = m_encryptionPnumToStatus[pnum];

        const uint_vec_t &pids = m_encryptionPnumToPids[pnum];
        if (!pids.empty())
        {
            uint enc_cnt[3] = { 0, 0, 0 };
            for (uint pid2 : pids)
            {
                CryptStatus stat = m_encryptionPidToInfo[pid2].m_status;
                enc_cnt[stat]++;

#if 0
                LOG(VB_GENERAL, LOG_DEBUG, LOC +
                    QString("\tpnum %1 PID 0x%2 status: %3")
                        .arg(pnum).arg(pid2,0,16) .arg(toString(stat)));
#endif
            }
            status = kEncUnknown;

            if (enc_cnt[kEncEncrypted])
                status = kEncEncrypted;
            else if (enc_cnt[kEncDecrypted] >= min((size_t) 2, pids.size()))
                status = kEncDecrypted;
        }

        if (status == m_encryptionPnumToStatus[pnum])
            continue; // program encryption status unchanged

        LOG(VB_RECORD, LOG_DEBUG, LOC + QString("Program %1 status: %2")
                .arg(pnum).arg(toString(status)));

        m_encryptionPnumToStatus[pnum] = status;

        bool encrypted = kEncUnknown == status || kEncEncrypted == status;
        m_listenerLock.lock();
        for (auto & listener : m_mpegListeners)
            listener->HandleEncryptionStatus(pnum, encrypted);
        m_listenerLock.unlock();

        if (kEncDecrypted == status)
            pnum_del_list.push_back(pnum);
    }

    for (size_t i = 0; i < pnum_del_list.size(); i++)
        RemoveEncryptionTestPIDs(pnums[i]);
}
