/*
Copyright (C) 2010-2012 Srivats P.

This file is part of "Ostinato"

This is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>
*/

#define __STDC_FORMAT_MACROS

#include "abstractport.h"

#include "../common/abstractprotocol.h"
#include "../common/streambase.h"
#include "devicemanager.h"
#include "packetbuffer.h"

#include <QString>
#include <QIODevice>

#include <inttypes.h>
#include <limits.h>
#include <math.h>

AbstractPort::AbstractPort(int id, const char *device)
{
    isUsable_ = true;
    data_.mutable_port_id()->set_id(id);
    data_.set_name(device);

    //! \todo (LOW) admin enable/disable of port
    data_.set_is_enabled(true);

    data_.set_is_exclusive_control(false);

    isSendQueueDirty_ = false;
    rateAccuracy_ = kHighAccuracy;
    linkState_ = OstProto::LinkStateUnknown;
    minPacketSetSize_ = 1;

    deviceManager_ = new DeviceManager(this);

    maxStatsValue_ = ULLONG_MAX; // assume 64-bit stats
    memset((void*) &stats_, 0, sizeof(stats_));
    resetStats();
}

AbstractPort::~AbstractPort()
{
    delete deviceManager_;
}    

void AbstractPort::init()
{
}    

bool AbstractPort::modify(const OstProto::Port &port)
{
    bool ret = true;

    //! \todo Use reflection to find out which fields are set
    if (port.has_is_exclusive_control())
    {
        bool val = port.is_exclusive_control();

        ret = setExclusiveControl(val);
        if (ret)
            data_.set_is_exclusive_control(val);
    }

    if (port.has_transmit_mode())
        data_.set_transmit_mode(port.transmit_mode());

    if (port.has_user_name()) {
        data_.set_user_name(port.user_name());
    }

    if (port.has_is_pkt_buf_size_enabled()) {
        data_.set_is_pkt_buf_size_enabled(port.is_pkt_buf_size_enabled());
    }

    if (port.has_pkt_buf_size()) {
        data_.set_pkt_buf_size(port.pkt_buf_size());
    }

    if (port.has_is_time_stamp_enabled()) {
        data_.set_is_time_stamp_enabled(port.is_time_stamp_enabled());
    }

    if (port.has_time_stamp_offset()) {
        data_.set_time_stamp_offset(port.time_stamp_offset());
    }

    if (port.has_time_stamp_size()) {
        data_.set_time_stamp_size(port.time_stamp_size());
    }

    if (port.has_is_nettest_enabled()) {
        data_.set_is_nettest_enabled(port.is_nettest_enabled());
    }

    if (port.has_nettest_stack_mode()) {
        data_.set_nettest_stack_mode(port.nettest_stack_mode());
    }

    if (port.has_nettest_hdr_offset()) {
        data_.set_nettest_hdr_offset(port.nettest_hdr_offset());
    }

    if (port.has_nettest_stream_id()) {
        data_.set_nettest_stream_id(port.nettest_stream_id());
    }

    if (port.has_is_nettest_error_check_enabled()) {
        data_.set_is_nettest_error_check_enabled(port.is_nettest_error_check_enabled());
    }

    modifyTransmitterConfig();
    modifyMonitorConfig();

    return ret;
}    

DeviceManager* AbstractPort::deviceManager()
{
    return deviceManager_;
}

StreamBase* AbstractPort::streamAtIndex(int index)
{
    Q_ASSERT(index < streamList_.size());
    return streamList_.at(index);
}

StreamBase* AbstractPort::stream(int streamId)
{
    for (int i = 0; i < streamList_.size(); i++)
    {
        if ((uint)streamId == streamList_.at(i)->id())
            return streamList_.at(i);
    }

    return NULL;
}

bool AbstractPort::addStream(StreamBase *stream)
{
    streamList_.append(stream);
    isSendQueueDirty_ = true;
    return true;
}

bool AbstractPort::deleteStream(int streamId)
{
    for (int i = 0; i < streamList_.size(); i++)
    {
        StreamBase *stream;

        if ((uint)streamId == streamList_.at(i)->id())
        {
            stream = streamList_.takeAt(i);
            delete stream;
            
            isSendQueueDirty_ = true;
            return true;
        }
    }

    return false;
}

void AbstractPort::addNote(QString note)
{
    QString notes = QString::fromStdString(data_.notes());

    note.prepend("<li>");
    note.append("</li>");

    if (notes.isEmpty())
        notes="<b>Limitation(s)</b><ul>";
    else
        notes.remove("</ul>");

    notes.append(note);
    notes.append("</ul>");

    data_.set_notes(notes.toStdString());
}

AbstractPort::Accuracy AbstractPort::rateAccuracy()
{
    return rateAccuracy_;
}

bool AbstractPort::setRateAccuracy(Accuracy accuracy)
{
    rateAccuracy_ = accuracy;
    return true;
}

void AbstractPort::updatePacketList()
{
    switch(data_.transmit_mode())
    {
    case OstProto::kSequentialTransmit:
        updatePacketListSequential();
        break;
    case OstProto::kInterleavedTransmit:
        updatePacketListInterleaved();
        break;
    default:
        Q_ASSERT(false); // Unreachable!!!
        break;
    }
}

void AbstractPort::updatePacketListSequential()
{
    long    sec = 0; 
    long    nsec = 0;

    qDebug("In %s", __FUNCTION__);

    // First sort the streams by ordinalValue
    qSort(streamList_.begin(), streamList_.end(), StreamBase::StreamLessThan);

    clearPacketList();

    for (int i = 0; i < streamList_.size(); i++)
    {
        if (streamList_[i]->isEnabled())
        {
            int len = 0;
            ulong n, x, y;
            ulong burstSize;
            double ibg = 0;
            quint64 ibg1 = 0, ibg2 = 0;
            quint64 nb1 = 0, nb2 = 0;
            double ipg = 0;
            quint64 ipg1 = 0, ipg2 = 0;
            quint64 npx1 = 0, npx2 = 0;
            quint64 npy1 = 0, npy2 = 0;
            quint64 loopDelay;
            ulong frameVariableCount = streamList_[i]->frameVariableCount();

            // We derive n, x, y such that
            // n * x + y = total number of packets to be sent

            switch (streamList_[i]->sendUnit())
            {
            case OstProto::StreamControl::e_su_bursts:
                burstSize = streamList_[i]->burstSize();
                x = AbstractProtocol::lcm(frameVariableCount, burstSize);
                n = ulong(burstSize * streamList_[i]->numBursts()) / x;
                y = ulong(burstSize * streamList_[i]->numBursts()) % x;
                if (streamList_[i]->burstRate() > 0)
                {
                    ibg = 1e9/double(streamList_[i]->burstRate());
                    ibg1 = quint64(ceil(ibg));
                    ibg2 = quint64(floor(ibg));
                    nb1  = quint64((ibg - double(ibg2)) * double(x));
                    nb2  = x - nb1;
                }
                loopDelay = ibg2;
                break;
            case OstProto::StreamControl::e_su_packets:
                x = frameVariableCount;
                n = 2;
                while (x < minPacketSetSize_) 
                    x = frameVariableCount*n++;
                n = streamList_[i]->numPackets() / x;
                y = streamList_[i]->numPackets() % x;
                burstSize = x + y;
                if (streamList_[i]->packetRate() > 0)
                {
                    ipg = 1e9/double(streamList_[i]->packetRate());
                    ipg1 = quint64(ceil(ipg));
                    ipg2 = quint64(floor(ipg));
                    npx1  = quint64((ipg - double(ipg2)) * double(x));
                    npx2  = x - npx1;
                    npy1  = quint64((ipg - double(ipg2)) * double(y));
                    npy2  = y - npy1;
                }
                loopDelay = ipg2;
                break;
            default:
                qWarning("Unhandled stream control unit %d",
                    streamList_[i]->sendUnit());
                continue;
            }

            qDebug("\nframeVariableCount = %lu", frameVariableCount);
            qDebug("n = %lu, x = %lu, y = %lu, burstSize = %lu",
                    n, x, y, burstSize);

            qDebug("ibg  = %g", ibg);
            qDebug("ibg1 = %" PRIu64, ibg1);
            qDebug("nb1  = %" PRIu64, nb1);
            qDebug("ibg2 = %" PRIu64, ibg2);
            qDebug("nb2  = %" PRIu64 "\n", nb2);

            qDebug("ipg  = %g", ipg);
            qDebug("ipg1 = %" PRIu64, ipg1);
            qDebug("npx1 = %" PRIu64, npx1);
            qDebug("npy1 = %" PRIu64, npy1);
            qDebug("ipg2 = %" PRIu64, ipg2);
            qDebug("npx2 = %" PRIu64, npx2);
            qDebug("npy2 = %" PRIu64 "\n", npy2);

            if (n > 1)
                loopNextPacketSet(x, n, 0, loopDelay);
            else if (n == 0)
                x = 0;

            for (uint j = 0; j < (x+y); j++)
            {
                
                if (j == 0 || frameVariableCount > 1)
                {
                    len = streamList_[i]->frameValue(
                            pktBuf_, sizeof(pktBuf_), j);
                }
                if (len <= 0)
                    continue;

                qDebug("q(%d, %d) sec = %lu nsec = %lu",
                        i, j, sec, nsec);

                appendToPacketList(sec, nsec, pktBuf_, len); 

                if ((j > 0) && (((j+1) % burstSize) == 0))
                {
                    nsec += (j < nb1) ? ibg1 : ibg2;
                    while (nsec >= long(1e9))
                    {
                        sec++;
                        nsec -= long(1e9);
                    }
                }
                else
                {
                    if (j < x)
                        nsec += (j < npx1) ? ipg1 : ipg2;
                    else
                        nsec += ((j-x) < npy1) ? ipg1 : ipg2;

                    while (nsec >= long(1e9))
                    {
                        sec++;
                        nsec -= long(1e9);
                    }
                }
            }

            switch(streamList_[i]->nextWhat())
            {
                case ::OstProto::StreamControl::e_nw_stop:
                    goto _stop_no_more_pkts;

                case ::OstProto::StreamControl::e_nw_goto_id:
                    /*! \todo (MED): define and use 
                    streamList_[i].d.control().goto_stream_id(); */

                    /*! \todo (MED): assumes goto Id is less than current!!!! 
                     To support goto to any id, do
                     if goto_id > curr_id then 
                         i = goto_id;
                         goto restart;
                     else
                         returnToQIdx = 0;
                     */

                    setPacketListLoopMode(true, 0, 
                            streamList_[i]->sendUnit() == 
                                StreamBase::e_su_bursts ? ibg1 : ipg1);
                    goto _stop_no_more_pkts;

                case ::OstProto::StreamControl::e_nw_goto_next:
                    break;

                default:
                    qFatal("---------- %s: Unhandled case (%d) -----------",
                            __FUNCTION__, streamList_[i]->nextWhat() );
                    break;
            }

        } // if (stream is enabled)
    } // for (numStreams)

_stop_no_more_pkts:
    isSendQueueDirty_ = false;
}

void AbstractPort::updatePacketListInterleaved()
{
    // TODO генарация набора пакетов
    int numStreams = 0;
    quint64 minGap = ULLONG_MAX;
    quint64 duration = quint64(1e9);
    QList<quint64> ibg1, ibg2;
    QList<quint64> nb1, nb2;
    QList<quint64> ipg1, ipg2;
    QList<quint64> np1, np2;
    QList<ulong> schedSec, schedNsec;
    QList<ulong> pktCount, burstCount;
    QList<ulong> burstSize;
    QList<bool> isVariable;
    QList<QByteArray> pktBuf;
    QList<ulong> pktLen;

    qDebug("In %s", __FUNCTION__);

    clearPacketList();
    if (streamList_.size() == 0)
    {
        isSendQueueDirty_ = false;
        return;
    }

    // First sort the streams by ordinalValue
    qSort(streamList_.begin(), streamList_.end(), StreamBase::StreamLessThan);

    // Подготовка плана последовательности пакетов
    for (int i = 0; i < streamList_.size(); i++)
    {
        if (!streamList_[i]->isEnabled())
            continue;

        double numBursts = 0;
        double numPackets = 0;

        quint64 _burstSize = 0;
        double ibg = 0;
        quint64 _ibg1 = 0, _ibg2 = 0;
        quint64 _nb1 = 0, _nb2 = 0;
        double ipg = 0;
        quint64 _ipg1 = 0, _ipg2 = 0;
        quint64 _np1 = 0, _np2 = 0;

        switch (streamList_[i]->sendUnit())
        {
        case OstProto::StreamControl::e_su_bursts:
            numBursts = streamList_[i]->burstRate();
            if (streamList_[i]->burstRate() > 0)
            {
                ibg = 1e9/double(streamList_[i]->burstRate());
                _ibg1 = quint64(ceil(ibg));
                _ibg2 = quint64(floor(ibg));
                _nb1 = quint64((ibg - double(_ibg2)) * double(numBursts));
                _nb2 = quint64(numBursts) - _nb1;
                _burstSize = streamList_[i]->burstSize();
            }
            break;
        case OstProto::StreamControl::e_su_packets:
            // TODO [3] pre-send processing
            // Число сгенерированных пакетов соотвутствует указанному числу в секунду
            // Необходимо добавить поле - число генерируемых пакетов, независимое от packetRate
//            numPackets = streamList_[i]->numPackets();
            numPackets = streamList_[i]->packetRate();      // число генерируемых пакетов
            if (streamList_[i]->packetRate() > 0)
            {
                // если межпакетный интервал представлен с долями нс, то он аппроксимируется двумя ближайшими целыми интервалами
                ipg = 1e9/double(streamList_[i]->packetRate()); // межпакетный интервал (в нс)
                _ipg1 = llrint(ceil(ipg));      // межпакетный интервал, округленный до целого вверх
                _ipg2 = quint64(floor(ipg));    // межпакетный интервал, округленный до целого вниз
                _np1 = quint64((ipg - double(_ipg2)) * double(numPackets)); // число пакетов с интервалом _ipg2
                _np2 = quint64(numPackets) - _np1;                          // число пакетов с интервалом _ipg1
                _burstSize = 1;
            }
            break;
        default:
            qWarning("Unhandled stream control unit %d",
                streamList_[i]->sendUnit());
            continue;
        }
        qDebug("numBursts = %g, numPackets = %g\n", numBursts, numPackets);

        qDebug("ibg  = %f", ibg);
        qDebug("ibg1 = %" PRIu64, _ibg1);
        qDebug("nb1  = %" PRIu64, _nb1);
        qDebug("ibg2 = %" PRIu64, _ibg2);
        qDebug("nb2  = %" PRIu64 "\n", _nb2);

        qDebug("ipg  = %f", ipg);
        qDebug("ipg1 = %" PRIu64, _ipg1);
        qDebug("np1  = %" PRIu64, _np1);
        qDebug("ipg2 = %" PRIu64, _ipg2);
        qDebug("np2  = %" PRIu64 "\n", _np2);


        if (_ibg2 && (_ibg2 < minGap))
            minGap = _ibg2;

        if (_ibg1 && (_ibg1 > duration))
            duration = _ibg1;

        ibg1.append(_ibg1);
        ibg2.append(_ibg2);

        nb1.append(_nb1);
        nb2.append(_nb1);

        burstSize.append(_burstSize);

        if (_ipg2 && (_ipg2 < minGap))
            minGap = _ipg2;

        if (_np1)
        {
            if (_ipg1 && (_ipg1 > duration))
                duration = _ipg1;
        }
        else
        {
            if (_ipg2 && (_ipg2 > duration))
                duration = _ipg2;
        }

        ipg1.append(_ipg1);
        ipg2.append(_ipg2);

        np1.append(_np1);
        np2.append(_np1);

        schedSec.append(0);
        schedNsec.append(0);

        pktCount.append(0);
        burstCount.append(0);

        if (streamList_[i]->isFrameVariable())  // если поля протокола изменяющиеся, то создается заготовка пустого пакета
        {
            isVariable.append(true);
            pktBuf.append(QByteArray());
            pktLen.append(0);
        }
        else    // если содержимое пакета не изменяющееся, то формируется фиксированный пакет, заполненный данными
        {
            isVariable.append(false);
            pktBuf.append(QByteArray());
            pktBuf.last().resize(kMaxPktSize);
            pktLen.append(streamList_[i]->frameValue(
                    (uchar*)pktBuf.last().data(), pktBuf.last().size(), 0));
        }

        numStreams++;
    } // for i

    qDebug("minGap   = %" PRIu64, minGap);
    qDebug("duration = %" PRIu64, duration);

    uchar* buf;
    int len;
    quint64 durSec = duration/ulong(1e9);
    quint64 durNsec = duration % ulong(1e9);
    quint64 sec = 0; 
    quint64 nsec = 0;
    quint64 lastPktTxSec = 0;
    quint64 lastPktTxNsec = 0;
    quint64 sumLen = 0;
    bool continueGenerate = false;

    // генерация последовательности пакетов в соответствии с планом
    do
    {
        for (int i = 0; i < numStreams; i++)
        {
            // If a packet is not scheduled yet, look at the next stream
            if ((schedSec.at(i) > sec) || (schedNsec.at(i) > nsec))
                continue;

            for (uint j = 0; j < burstSize[i]; j++)
            {
                // формируем очередной пакет
                if (isVariable.at(i))
                {
                    buf = pktBuf_;
                    len = streamList_[i]->frameValue(pktBuf_, sizeof(pktBuf_), // заполняем изменяющиеся поля
                            pktCount[i]);
                }
                else
                {
                    buf = (uchar*) pktBuf.at(i).data();
                    len = pktLen.at(i);
                }

                if (len <= 0)
                    continue;

                //qDebug("q(%d) sec = %" PRIu64 " nsec = %" PRIu64, i, sec, nsec);
                appendToPacketList(sec, nsec, buf, len);    // добавляем пакет в список
                lastPktTxSec = sec;
                lastPktTxNsec = nsec;
                sumLen += len;

                pktCount[i]++;
                //qDebug("********* pktCount[%d] = %d    burstSize[%d] = %d", i, pktCount[i], i, burstSize[i]);
                schedNsec[i] += (pktCount.at(i) < np1.at(i)) ? 
                    ipg1.at(i) : ipg2.at(i);
                while (schedNsec.at(i) >= 1e9)
                {
                    schedSec[i]++;
                    schedNsec[i] -= long(1e9);
                }
            }

            burstCount[i]++;
            schedNsec[i] += (burstCount.at(i) < nb1.at(i)) ? 
                    ibg1.at(i) : ibg2.at(i);
            while (schedNsec.at(i) >= 1e9)
            {
                schedSec[i]++;
                schedNsec[i] -= long(1e9);
            }
        } 

        nsec += minGap;
        while (nsec >= 1e9)
        {
            sec++;
            nsec -= long(1e9);
        }

        // проверка условия продолжения генерации пакетов
        // в зависимости от режима пакеты генерируются до достижения 1 с или до максимального размера буфера
        if (data_.is_pkt_buf_size_enabled())
            continueGenerate = sumLen < data_.pkt_buf_size()*1000;
        else
            continueGenerate = ((sec < durSec) || (nsec < durNsec));

    } while (continueGenerate);
//    } while ((sec < durSec) || (nsec < durNsec));

    qint64 delaySec = durSec - lastPktTxSec;
    qint64 delayNsec = durNsec - lastPktTxNsec;
    while (delayNsec < 0)
    {
        delayNsec += long(1e9);
        delaySec--;
    }
    qDebug("loop Delay = %" PRId64 "/%" PRId64, delaySec, delayNsec);
    setPacketListLoopMode(true, delaySec, delayNsec);
    isSendQueueDirty_ = false;
}

void AbstractPort::stats(PortStats *stats)
{
    stats->rxPkts = (stats_.rxPkts >= epochStats_.rxPkts) ?
                        stats_.rxPkts - epochStats_.rxPkts :
                        stats_.rxPkts + (maxStatsValue_ - epochStats_.rxPkts);
    stats->rxBytes = (stats_.rxBytes >= epochStats_.rxBytes) ?
                        stats_.rxBytes - epochStats_.rxBytes :
                        stats_.rxBytes + (maxStatsValue_ - epochStats_.rxBytes);
    stats->rxPps = stats_.rxPps;
    stats->rxBps = stats_.rxBps;

    stats->txPkts = (stats_.txPkts >= epochStats_.txPkts) ?
                        stats_.txPkts - epochStats_.txPkts :
                        stats_.txPkts + (maxStatsValue_ - epochStats_.txPkts);
    stats->txBytes = (stats_.txBytes >= epochStats_.txBytes) ?
                        stats_.txBytes - epochStats_.txBytes :
                        stats_.txBytes + (maxStatsValue_ - epochStats_.txBytes);
    stats->txPps = stats_.txPps;
    stats->txBps = stats_.txBps;

    stats->rxDrops = (stats_.rxDrops >= epochStats_.rxDrops) ?
                        stats_.rxDrops - epochStats_.rxDrops :
                        stats_.rxDrops + (maxStatsValue_ - epochStats_.rxDrops);
    stats->rxErrors = (stats_.rxErrors >= epochStats_.rxErrors) ?
                        stats_.rxErrors - epochStats_.rxErrors :
                        stats_.rxErrors + (maxStatsValue_ - epochStats_.rxErrors);
    stats->rxFifoErrors = (stats_.rxFifoErrors >= epochStats_.rxFifoErrors) ?
                        stats_.rxFifoErrors - epochStats_.rxFifoErrors :
                        stats_.rxFifoErrors + (maxStatsValue_ - epochStats_.rxFifoErrors);
    stats->rxFrameErrors = (stats_.rxFrameErrors >= epochStats_.rxFrameErrors) ?
                        stats_.rxFrameErrors - epochStats_.rxFrameErrors :
                        stats_.rxFrameErrors + (maxStatsValue_ - epochStats_.rxFrameErrors);

    stats->ntPkts = stats_.ntPkts;
    stats->ntBytes = stats_.ntBytes;
    stats->ntBps = stats_.ntBps;
    stats->ntAvgDelayUs = stats_.ntAvgDelayUs;
    stats->ntMmoDelayUs = stats_.ntMmoDelayUs;
    stats->ntMinDelayUs = stats_.ntMinDelayUs;
    stats->ntMaxDelayUs = stats_.ntMaxDelayUs;
    stats->ntAvgJitterUs = stats_.ntAvgJitterUs;
    stats->ntMmoJitterUs = stats_.ntMmoJitterUs;
    stats->ntMinJitterUs = stats_.ntMinJitterUs;
    stats->ntMaxJitterUs = stats_.ntMaxJitterUs;

    stats->ntLossCount = stats_.ntLossCount;
    stats->ntOutOfWndCount = stats_.ntOutOfWndCount;
    stats->ntLossKoeff = stats_.ntLossKoeff;
    stats->ntOutOfWndKoeff = stats_.ntOutOfWndKoeff;
    stats->ntMmoLossKoeff = stats_.ntMmoLossKoeff;
    stats->ntMmoOutOfWndKoeff = stats_.ntMmoOutOfWndKoeff;
}

void AbstractPort::clearDeviceNeighbors()
{
    deviceManager_->clearDeviceNeighbors();
    isSendQueueDirty_ = true;
}

void AbstractPort::resolveDeviceNeighbors()
{
    // For a user triggered 'Resolve Neighbors', the behaviour we want is
    //   IP not in cache - send ARP/NDP request
    //   IP present in cache, but unresolved - re-send ARP/NDP request
    //   IP present in cache and resolved - don't sent ARP/NDP
    //
    // Device does not resend ARP/NDP requests if the IP address is
    // already present in the cache, irrespective of whether it is
    // resolved or not (this is done to avoid sending duplicate requests).
    //
    // So, to get the behaviour we want, let's clear all unresolved neighbors
    // before calling resolve
    deviceManager_->clearDeviceNeighbors(Device::kUnresolvedNeighbors);

    // Resolve gateway for each device first ...
    deviceManager_->resolveDeviceGateways();

    // ... then resolve neighbor for each unique frame of each stream
    // NOTE:
    // 1. All the frames may have the same destination ip,but may have
    // different source ip so may belong to a different emulated device;
    // so we cannot optimize and send only one ARP
    // 2. For a unidirectional stream, at egress, this will create ARP
    // entries on the DUT for each of the source addresses
    //
    // TODO(optimization): Identify if stream does not vary in srcIp or dstIp
    // - in which case resolve for only one frame of the stream
    for (int i = 0; i < streamList_.size(); i++)
    {
        const StreamBase *stream = streamList_.at(i);
        int frameCount = stream->frameVariableCount();

        for (int j = 0; j < frameCount; j++) {
            // we need the packet contents only uptil the L3 header
            int pktLen = stream->frameValue(pktBuf_, kMaxL3PktSize, j);
            if (pktLen) {
                PacketBuffer pktBuf(pktBuf_, pktLen);
                deviceManager_->resolveDeviceNeighbor(&pktBuf);
            }
        }
    }
    isSendQueueDirty_ = true;
}

quint64 AbstractPort::deviceMacAddress(int streamId, int frameIndex)
{
    // we need the packet contents only uptil the L3 header
    StreamBase *s = stream(streamId);
    int pktLen = s->frameValue(pktBuf_, kMaxL3PktSize, frameIndex);

    if (pktLen) {
        PacketBuffer pktBuf(pktBuf_, pktLen);
        return deviceManager_->deviceMacAddress(&pktBuf);
    }

    return 0;
}

quint64 AbstractPort::neighborMacAddress(int streamId, int frameIndex)
{
    // we need the packet contents only uptil the L3 header
    StreamBase *s = stream(streamId);
    int pktLen = s->frameValue(pktBuf_, kMaxL3PktSize, frameIndex);

    if (pktLen) {
        PacketBuffer pktBuf(pktBuf_, pktLen);
        return deviceManager_->neighborMacAddress(&pktBuf);
    }

    return 0;
}

