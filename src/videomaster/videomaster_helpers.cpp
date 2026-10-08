/*
 * SPDX-FileCopyrightText: Copyright (c) DELTACAST.TV. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at * * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "videomaster_helpers.hpp"

#include <utility>
#include <VideoMasterCppApi/to_string.hpp>
#include <VideoMasterCppApi/exception.hpp>
#include <VideoMasterCppApi/helper/sdi.hpp>
#include <sstream>

template<class... Ts>
struct overloaded : Ts... { using Ts::operator()...; };
template<class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

using namespace Deltacast::Wrapper;
using namespace Deltacast::Wrapper::Helper;

namespace Application::Helper
{
    VHD_STREAMTYPE rx_index_to_streamtype(unsigned int rx_index)
    {
        switch (rx_index)
        {
        case 0: return VHD_ST_RX0;
        case 1: return VHD_ST_RX1;
        case 2: return VHD_ST_RX2;
        case 3: return VHD_ST_RX3;
        case 4: return VHD_ST_RX4;
        case 5: return VHD_ST_RX5;
        case 6: return VHD_ST_RX6;
        case 7: return VHD_ST_RX7;
        case 8: return VHD_ST_RX8;
        case 9: return VHD_ST_RX9;
        case 10: return VHD_ST_RX10;
        case 11: return VHD_ST_RX11;
        default:
            throw std::invalid_argument("Invalid RX index");
        }
    }

    static VHD_CHANNELTYPE stream_type_to_channel_type(Board& board, VHD_STREAMTYPE stream_type)
    {
        switch (stream_type)
        {
            case VHD_ST_RX0: return board.rx(0).type();
            case VHD_ST_RX1: return board.rx(1).type();
            case VHD_ST_RX2: return board.rx(2).type();
            case VHD_ST_RX3: return board.rx(3).type();
            case VHD_ST_RX4: return board.rx(4).type();
            case VHD_ST_RX5: return board.rx(5).type();
            case VHD_ST_RX6: return board.rx(6).type();
            case VHD_ST_RX7: return board.rx(7).type();
            case VHD_ST_RX8: return board.rx(8).type();
            case VHD_ST_RX9: return board.rx(9).type();
            case VHD_ST_RX10: return board.rx(10).type();
            case VHD_ST_RX11: return board.rx(11).type();
            default:
                throw std::invalid_argument("Invalid stream type");
        }
    }

    TechStream open_stream(Board& board, VHD_STREAMTYPE stream_type)
    {
        auto channel_type = stream_type_to_channel_type(board, stream_type);
        switch (channel_type)
        {
            case VHD_CHNTYPE_HDSDI:
            case VHD_CHNTYPE_3GSDI:
            case VHD_CHNTYPE_12GSDI:
                return std::move(board.sdi().open_stream(stream_type, VHD_SDI_STPROC_DISJOINED_VIDEO));
            case VHD_CHNTYPE_HDMI_TMDS:
            case VHD_CHNTYPE_HDMI_FRL3:
            case VHD_CHNTYPE_HDMI_FRL4:
            case VHD_CHNTYPE_HDMI_FRL5:
            case VHD_CHNTYPE_HDMI_FRL6:
            case VHD_CHNTYPE_DISPLAYPORT:
                return std::move(board.dv().open_stream(stream_type, VHD_DV_STPROC_DISJOINED_VIDEO));
            default:
                throw std::invalid_argument("Invalid channel type");
        }
    }

    Stream& to_base_stream(TechStream& stream)
    {
        return std::visit(overloaded{
            [](SdiStream& sdi_stream) -> Stream& { return sdi_stream; },
            [](DvStream& dv_stream) -> Stream& { return dv_stream; }
        }, stream);
    }

    void configure_stream(TechStream& stream, const SignalInformation& signal_information)
    {
        std::visit(overloaded{
            [&signal_information](SdiStream& sdi_stream)
            {
                const SdiSignalInformation& sdi_signal_information = std::get<SdiSignalInformation>(signal_information);
                sdi_stream.set_video_standard(sdi_signal_information.video_standard);
                sdi_stream.set_interface(sdi_signal_information.video_interface);
            },
            [&signal_information](DvStream& dv_stream)
            {
                const DvSignalInformation& dv_signal_information = std::get<DvSignalInformation>(signal_information);
                dv_stream.set_active_width(dv_signal_information.width);
                dv_stream.set_active_height(dv_signal_information.height);
                dv_signal_information.progressive ? dv_stream.set_progressive() : dv_stream.set_interlaced();
                dv_stream.set_frame_rate(dv_signal_information.framerate);
                dv_stream.set_cable_color_space(dv_signal_information.cable_color_space);
            }
        }, stream);
    }

    std::string get_information_string(const SignalInformation& signal_information, const std::string& prefix) {
        std::ostringstream out;

        std::visit(overloaded{
            [&out, &prefix](const SdiSignalInformation& sdi_signal_info)
            {
                out << prefix << "Video standard: "
                    << to_pretty_string(sdi_signal_info.video_standard) << ", ";
                out << prefix << "Clock divisor: "
                    << to_pretty_string(sdi_signal_info.clock_divisor) << ", ";
                out << prefix << "Interface: "
                    << to_pretty_string(sdi_signal_info.video_interface);
            },
            [&out, &prefix](const DvSignalInformation& dv_signal_info)
            {
                out << prefix
                    << dv_signal_info.width << "x"
                    << dv_signal_info.height
                    << (dv_signal_info.progressive ? "p" : "i")
                    << dv_signal_info.framerate << ", ";

                out << prefix
                    << to_pretty_string(dv_signal_info.cable_color_space) << ", ";
                out << prefix
                    << to_pretty_string(dv_signal_info.cable_sampling);
            }
            }, signal_information);

        return out.str();
    }

    SignalInformation detect_information(TechStream& stream)
    {
        return std::visit(overloaded{
            [](SdiStream& sdi_stream) -> SignalInformation
            {
                return SdiSignalInformation{sdi_stream.video_standard(), sdi_stream.clock_divisor(), sdi_stream.interface()};
            },
            [](DvStream& dv_stream) -> SignalInformation
            {
                return DvSignalInformation{dv_stream.active_width(), dv_stream.active_height(), !dv_stream.interlaced(), dv_stream.frame_rate()
                                            , dv_stream.cable_color_space(), dv_stream.cable_sampling()};
            }
        }, stream);
    }

    VideoCharacteristics get_video_characteristics(const SignalInformation& signal_information)
    {
        return std::visit(overloaded{
            [](const SdiSignalInformation& sdi_signal_info) -> VideoCharacteristics
            {
                return Sdi::video_standard_to_characteristics(sdi_signal_info.video_standard);
            },
            [](const DvSignalInformation& dv_signal_info) -> VideoCharacteristics
            {
                return { dv_signal_info.width, dv_signal_info.height, !dv_signal_info.progressive, dv_signal_info.framerate };
            }
        }, signal_information);
    }
}
