// -*- Mode: c++ -*-
// Copyright (c) 2003-2004, Daniel Thor Kristjansson
#include "scanstreamdata.h"
#include "atsctables.h"
#include "dvbtables.h"

ScanStreamData::ScanStreamData(bool no_default_pid) :
    MPEGStreamData(-1, -1, true),
    ATSCStreamData(-1, -1, -1, true),
    DVBStreamData(0, 0, -1, -1, true),
    m_noDefaultPid(no_default_pid)
{
    if (m_noDefaultPid)
        m_pidsListening.clear();
}

ScanStreamData::~ScanStreamData() { ; }

/** \fn ScanStreamData::IsRedundant(uint,const PSIPTable&) const
 *  \brief Returns true if table already seen.
 */
bool ScanStreamData::IsRedundant(uint pid, const PSIPTable &psip) const
{
    return (ATSCStreamData::IsRedundant(pid,psip) ||
            DVBStreamData::IsRedundant(pid,psip));
}

/** \fn ScanStreamData::HandleTables(uint, const PSIPTable&)
 *  \brief Processes PSIP tables
 */
bool ScanStreamData::HandleTables(uint pid, const PSIPTable &psip)
{
    bool h0 = ATSCStreamData::HandleTables(pid, psip);
    bool h1 = DVBStreamData::HandleTables(pid, psip);
    return h0 || h1;
}

void ScanStreamData::Reset(void)
{
    MPEGStreamData::Reset(-1);
    ATSCStreamData::Reset(-1,-1);
    DVBStreamData::Reset(0,0,-1);

    if (m_noDefaultPid)
    {
        m_pidsListening.clear();
        return;
    }

    AddListeningPID(MPEG_PAT_PID);
    AddListeningPID(ATSC_PSIP_PID);
    AddListeningPID(DVB_NIT_PID);
    AddListeningPID(DVB_SDT_PID);
    if (m_dvbUkFreesatSi)
        AddListeningPID(FREESAT_SI_PID);
}

QString ScanStreamData::GetSIStandard(const QString& guess) const
{
    if (HasCachedMGT())
        return "atsc";

    if (HasCachedAnyNIT())
        return "dvb";

    QMutexLocker locker(&m_cacheLock);

    pmt_cache_t::const_iterator it = m_cachedPmts.begin();
    for (; it != m_cachedPmts.end(); ++it)
    {
        ProgramMapTable *pmt = *it;

        for (uint i = 0; (guess != "dvb") && (i < pmt->StreamCount()); i++)
        {
            if (StreamID::OpenCableVideo == pmt->StreamType(i))
                return "opencable";
        }

        desc_list_t descs = MPEGDescriptor::ParseOnlyInclude(
            pmt->ProgramInfo(), pmt->ProgramInfoLength(),
            DescriptorID::registration);

        for (auto & desc : descs)
        {
            RegistrationDescriptor reg(desc);
            if (!reg.IsValid())
                continue;
            if (reg.FormatIdentifierString() == "SCTE")
                return "opencable";
        }
    }

    return "mpeg";
}


bool ScanStreamData::DeleteCachedTable(const PSIPTable *psip) const
{
    if (!psip)
        return false;

    if (ATSCStreamData::DeleteCachedTable(psip))
        return true;
    return DVBStreamData::DeleteCachedTable(psip);
}
