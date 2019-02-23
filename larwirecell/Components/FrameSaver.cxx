/*
 */

#include "FrameSaver.h"
//#include "art/Framework/Principal/Handle.h" 

#include "lardataobj/RecoBase/Wire.h"
#include "lardataobj/RawData/RawDigit.h"

#include "art/Framework/Principal/Event.h"
#include "art/Framework/Core/EDProducer.h"

#include "WireCellIface/IFrame.h"
#include "WireCellIface/ITrace.h"
#include "WireCellUtil/NamedFactory.h"

// it would be nice to remove this dependency since it is needed only
// to add bogus information to raw::RawDigit.  
#include "larevt/CalibrationDBI/Interface/DetPedestalService.h"
#include "larevt/CalibrationDBI/Interface/DetPedestalProvider.h"

#include <algorithm>
#include <map>

WIRECELL_FACTORY(wclsFrameSaver, wcls::FrameSaver,
		 wcls::IArtEventVisitor, WireCell::IFrameFilter)


using namespace wcls;
using namespace WireCell;

FrameSaver::FrameSaver()
    : m_frame(nullptr)
    , m_nticks(0)
{
}

FrameSaver::~FrameSaver()
{
}


WireCell::Configuration FrameSaver::default_configuration() const
{
    Configuration cfg;
    cfg["anode"] = "AnodePlane"; 

    // If true, truncate frame waveforms and save as raw::RawDigit,
    // else leave as floating point recob::Wire
    cfg["digitize"] = false;

    // If true, save recob::Wires as sparse (true zero suppressed).
    // Note, this sparsification is performed independently from if
    // the input IFrame itself is sparse or not.
    cfg["sparse"] = true;

    // If digitize, raw::RawDigit has slots for pedestal mean and
    // sigma.  Legacy/obsolete code stuff unrelated values into these
    // slots.  If pedestal_mean is a number, it will be stuffed.  If
    // it is the string "fiction" then the DetPedestalService will be
    // used to set some unrelated pedestal value.
    cfg["pedestal_mean"] = 0.0;
    // Stuff some value into pedestal sigma.  This value has nothing
    // to do with the produced NF'ed waveforms.
    cfg["pedestal_sigma"] = 0.0;

    // frames to output, if any
    cfg["frame_tags"] = Json::arrayValue;
    cfg["frame_scale"] = 1.0; 	// multiply this number to all
				// waveform samples.  If list, then
				// one number per frame tags.
    cfg["nticks"] = m_nticks; // if nonzero, force number of ticks in output waveforms. 

    // Summaries to output, if any
    cfg["summary_tags"] = Json::arrayValue;
    cfg["summary_scale"] = 1.0;
    // Sumaries are *per trace* quantities coming in but it is likely
    // that some (most?) consumers of the output will expect *per
    // channel* quantities.  Aggregating by channel requires some
    // operator to be applied to the sequence of summary values from a
    // common channel.  The operator is defined as an object keyed by
    // the summary tag.  Values may be "set" which will simply save a
    // summary value to the output element (last one from a channel
    // wins) or "sum" (the default) which will add up the values.
    cfg["summary_operator"] = Json::objectValue;

    // Names of channel mask maps to save, if any.
    cfg["chanmaskmaps"] = Json::arrayValue;

    return cfg;
}

static float summary_sum(const std::vector<float> &tsvals) {
    return std::accumulate(tsvals.begin(), tsvals.end(), 0);
}
static float summary_set(const std::vector<float> &tsvals) {
    if (tsvals.empty()) {
        return 0.0; 
    }
    return tsvals.back();
}

void FrameSaver::configure(const WireCell::Configuration& cfg)
{
    const std::string anode_tn = cfg["anode"].asString();
    if (anode_tn.empty()) {
        THROW(ValueError() << errmsg{"FrameSaver requires an anode plane"});
    }

    WireCell::IAnodePlane::pointer anode = Factory::find_tn<IAnodePlane>(anode_tn);
    for (auto chid : anode->channels()) {
        // geo::kU, geo::kV, geo::kW
        auto wpid = anode->resolve(chid);
        geo::View_t view;
        switch(wpid.layer()) {
        case WireCell::kUlayer:
            view = geo::kU;
            break;
        case WireCell::kVlayer:
            view = geo::kV;
            break;
        case WireCell::kWlayer:
            view = geo::kW;
            break;
        default:
            view = geo::kUnknown;
        }
        m_chview[chid] = view;
    }

    m_digitize = get(cfg, "digitize", false);
    m_sparse = get(cfg, "sparse", true);

    m_cmms = cfg["chanmaskmaps"];

    m_pedestal_mean = cfg["pedestal_mean"];
    m_pedestal_sigma = get(cfg, "pedestal_sigma", 0.0);

    if (!cfg["frame_scale"].isNull()) {
	m_frame_scale.clear();
	auto jscale = cfg["frame_scale"];
	m_frame_tags.clear();
	auto jtags = cfg["frame_tags"];
	for (auto jtag : jtags) {
	    std::string tag = jtag.asString();

	    // get any waveform scaling for this frame
	    const int ind = m_frame_tags.size();
	    double scale = 1.0;
	    //std::cerr << "JSCALE:" << jscale << std::endl;
	    if (!jscale.isNull()) {
		if (jscale.isArray()) {
		    scale = jscale[ind].asDouble();
		}
		else {
		    scale = jscale.asDouble();
		}
	    }

	    m_frame_scale.push_back(scale);
	    m_frame_tags.push_back(tag);
	}
	m_nticks = get(cfg, "nticks", m_nticks);
    }

    auto jso = cfg["summary_operator"];

    if (! cfg["summary_tags"].isNull()) {
	m_summary_scale.clear();
	auto jscale = cfg["summary_scale"];
	m_summary_tags.clear();
	auto jtags = cfg["summary_tags"];
	for (auto jtag : jtags) {
	    std::string tag = jtag.asString();

	    const int ind = m_summary_tags.size();
	    double scale = 1.0;
	    if (!jscale.isNull()) {
		if (jscale.isArray()) {
		    scale = jscale[ind].asDouble();
		}
		else {
		    scale = jscale.asDouble();
		}
	    }

	    m_summary_tags.push_back(tag);
	    m_summary_scale.push_back(scale);
            auto so = get<std::string>(jso, tag, "sum");
            if (so == "set") {
                m_summary_operators[tag] = summary_set;
            }
            else {
                m_summary_operators[tag] = summary_sum;
            }
	}
    }
}

void FrameSaver::produces(art::EDProducer* prod)
{
    assert(prod);

    for (auto tag : m_frame_tags) {
	if (!m_digitize) {
	    std::cerr << "wclsFrameSaver: promising to produce recob::Wires named \"" 
		      << tag << "\"\n";
	    prod->produces< std::vector<recob::Wire> >(tag);
	}
	else {
	    std::cerr << "wclsFrameSaver: promising to produce raw::RawDigits named \"" 
		      << tag << "\"\n";
	    prod->produces< std::vector<raw::RawDigit> >(tag);
	}
    }
    for (auto tag : m_summary_tags) {
	std::cerr << "wclsFrameSaver: promising to produce channel summary named \"" 
		  << tag << "\"\n";
	prod->produces< std::vector<double> >(tag);
    }
    for (auto cmm : m_cmms) {
	const std::string cmm_name = cmm.asString();
	std::cerr << "wclsFrameSaver: promising to produce channel masks named \"" 
		  << cmm_name << "\"\n";
	prod->produces< channel_list > (cmm_name + "channels");
	prod->produces< channel_masks > (cmm_name + "masks");
    }
}


static
void tagged_traces(IFrame::pointer frame, std::string tag, ITrace::vector& ret)
{
    auto const& all_traces = frame->traces();
    const auto& ttinds = frame->tagged_traces(tag);
    if (ttinds.size()) {
        for (size_t index: ttinds) {
            ret.push_back(all_traces->at(index));
        }
        return;
    }
    auto ftags = frame->frame_tags();
    if (std::find(ftags.begin(), ftags.end(), tag) == ftags.end()) {
        return;
    }
    ret.insert(ret.begin(), all_traces->begin(), all_traces->end());
}


typedef std::unordered_map<int, ITrace::vector> traces_bychan_t;
static void traces_bychan(ITrace::vector& traces, traces_bychan_t& ret)
{
    for (auto trace : traces) {
        int chid = trace->channel();
        ret[chid].push_back(trace);
    }
}


// Issolate some silly legacy shenanigans to keep the rest of the code
// blissfully ignorant of the evilness this implies.
struct PU {
    Json::Value pu;

    PU(Json::Value pu) : pu(pu) { }

    float operator()(int chid) {
	if (pu.isNumeric()) {
	    return pu.asFloat();
	}
	if (pu.asString() == "fiction") {
	    art::ServiceHandle<lariov::DetPedestalService> dps;
	    const auto& pv = dps->GetPedestalProvider();
	    return pv.PedMean(chid);
	}
	return 0.0;
    }
};
    
	

void FrameSaver::save_as_raw(art::Event & event)
{
    size_t nftags = m_frame_tags.size();
    for (size_t iftag = 0; iftag < nftags; ++iftag) {
        std::string ftag = m_frame_tags[iftag];
        std::cerr << "wclsFrameSaver: saving raw::RawDigits tagged \"" << ftag << "\"\n";

        double scale = m_frame_scale[iftag];

        ITrace::vector traces;
	tagged_traces(m_frame, ftag, traces);
        traces_bychan_t bychan;
        traces_bychan(traces, bychan);

        std::unique_ptr<std::vector<raw::RawDigit> > out(new std::vector<raw::RawDigit>);

        for (auto chv : m_chview) {
            const int chid = chv.first;
            const auto& traces = bychan[chid];
            const size_t ntraces = traces.size();

            int tbin = 0;
            std::vector<float> charge;
            if (ntraces) {
                auto trace = traces[0];
                tbin = trace->tbin();
                charge = trace->charge();
            }
            // charge may be empty here

            // enforce number of ticks if we are so configured.
            size_t ncharge = charge.size();
            int nticks = tbin + ncharge;
            if (m_nticks) {	// force output waveform size
                if (m_nticks < nticks) {
                    ncharge  = m_nticks - tbin;
                }
                nticks = m_nticks;
            }
            raw::RawDigit::ADCvector_t adcv(nticks);
            for (size_t ind=0; ind<ncharge; ++ind) {
                adcv[tbin+ind] = scale * charge[ind]; // scale + truncate/redigitize
            }
            out->emplace_back(raw::RawDigit(chid, nticks, adcv, raw::kNone));
            if (m_pedestal_mean.asString() == "native") {
                short baseline = Waveform::most_frequent(adcv);
                out->back().SetPedestal(baseline, m_pedestal_sigma);
            }
            else {
                PU pu(m_pedestal_mean);
                out->back().SetPedestal(pu(chid), m_pedestal_sigma);
            }
        }
        event.put(std::move(out), ftag);
    }
}



void FrameSaver::save_as_cooked(art::Event & event)
{
    size_t nftags = m_frame_tags.size();
    for (size_t iftag = 0; iftag < nftags; ++iftag) {
        std::string ftag = m_frame_tags[iftag];
        std::cerr << "wclsFrameSaver: saving recob::Wires tagged \"" << ftag << "\"\n";

        double scale = m_frame_scale[iftag];

        ITrace::vector traces;
	tagged_traces(m_frame, ftag, traces);
        traces_bychan_t bychan;
        traces_bychan(traces, bychan);

        std::unique_ptr<std::vector<recob::Wire> > outwires(new std::vector<recob::Wire>);

        for (auto chv : m_chview) {
            const int chid = chv.first;
            const auto& traces = bychan[chid];

            recob::Wire::RegionsOfInterest_t rois(m_nticks);

            for (const auto& trace : traces) {
                const int tbin = trace->tbin();
                const auto& charge = trace->charge();
                
                auto beg = charge.begin();
                const auto first = beg;
                auto end = charge.end();
                if (m_nticks) { // user wants max waveform size
                    if (tbin >= m_nticks) {
                        beg = end;
                    }
                    else {
                        int backup = tbin + charge.size() - m_nticks;
                        if (backup > 0) {
                            end -= backup;
                        }
                    }
                }
                if (beg >= end) {
                    continue;
                }
                if (!m_sparse) {
                    std::vector<float> scaled(beg,end);
                    for (size_t i=0; i<scaled.size(); ++i) scaled[i] *= scale;
                    // prefer combine_range() but it segfaults.
                    rois.add_range(tbin, scaled.begin(), scaled.end());
                    continue;
                }
                // sparsify trace whether or not it may itself already
                // represents a sparse ROI
                while (true) {
                    beg = std::find_if(beg, end, [](float v){return v!= 0.0;});
                    if (beg == end) {
                        break;
                    }
                    auto mid = std::find_if(beg, end, [](float v){return v == 0.0;});
                    std::vector<float> scaled(beg, mid);
                    for (int ind=0; ind<mid-beg; ++ind) {
                        scaled[ind] *= scale;
                    }
                    rois.add_range(tbin + beg-first, scaled.begin(), scaled.end());
                    beg = mid;
                }
            }

	    const geo::View_t view = chv.second;
	    outwires->emplace_back(recob::Wire(rois, chid, view));
	}
	event.put(std::move(outwires), ftag);
    } // loop over tags
}


void FrameSaver::save_summaries(art::Event & event)
{
    const int ntags = m_summary_tags.size();
    if (0 == ntags) {
        return;                 // no tags
    }

    const size_t nchans = m_chview.size();

    // for each summary
    for (int tag_ind=0; tag_ind<ntags; ++tag_ind) {
        // The scale set for the tag.
        const double scale = m_summary_scale[tag_ind];

        std::unique_ptr<std::vector<double> > outsum(new std::vector<double>(nchans, 0.0));


        // The "summary" and "traces" vectors of the same tag are
        // synced, element-by-element.  Each element corresponds to
        // one trace (ROI).  No particular order or correlation by
        // channel exists, and that's what the rest of this code
        // creates.
	auto tag = m_summary_tags[tag_ind];
        const auto& summary = m_frame->trace_summary(tag);
        ITrace::vector traces;
        tagged_traces(m_frame, tag, traces);
        const size_t ntraces = traces.size();

        std::unordered_map<int, std::vector<float> > bychan;
        for (size_t itrace=0; itrace < ntraces; ++itrace) {
            const int chid = traces[itrace]->channel();
            const double summary_value = summary[itrace];
            bychan[chid].push_back(summary_value);
        }
        auto oper = m_summary_operators[tag];


        size_t chanind=0;
        for (auto chv : m_chview) {
            const int chid = chv.first;
            const float val = oper(bychan[chid]);
            outsum->at(chanind) = val * scale;
            ++chanind;
        }
        event.put(std::move(outsum), tag);
    }
}

void FrameSaver::save_cmms(art::Event & event)
{
    if (m_cmms.isNull()) {
        return;
    }
    if (!m_cmms.isArray()) {
	std::cerr << "wclsFrameSaver: wrong type for configuration array of channel mask maps to save\n";
	return;
    }
    for (auto jcmm : m_cmms) {
	std::string name = jcmm.asString();
	std::unique_ptr< channel_list > out_list(new channel_list);
	std::unique_ptr< channel_masks > out_masks(new channel_masks);

    	auto cmm = m_frame->masks();
	auto it = cmm.find(name);
	if (it == cmm.end()) {
	    std::cerr << "wclsFrameSaver: failed to find requested channel masks \"" << name << "\"\n";
	    continue;
	}
	for (auto cmit : it->second) { // int->vec<pair<int,int>>
	    out_list->push_back(cmit.first);
	    for (auto be : cmit.second) {
		out_masks->push_back(cmit.first);
		out_masks->push_back(be.first);
		out_masks->push_back(be.second);
	    }
	}

	if (out_list->empty()) {
	    std::cerr << "wclsFrameSaver: found empty channel masks for \"" << name << "\"\n";
	}
	event.put(std::move(out_list), name + "channels");
	event.put(std::move(out_masks), name + "masks");
    }
}


void FrameSaver::save_empty(art::Event& event)
{
    // art (apparently?) requires something to be saved if a produces() is promised.
    std::cerr << "wclsFrameSaver: saving empty frame to art::Event\n";

    for (auto ftag : m_frame_tags) {
        if (m_digitize) {
            std::unique_ptr<std::vector<raw::RawDigit> > out(new std::vector<raw::RawDigit>);
            event.put(std::move(out), ftag);

        }
        else {
            std::unique_ptr<std::vector<recob::Wire> > outwires(new std::vector<recob::Wire>);
            event.put(std::move(outwires), ftag);
        }
    }

    for (auto stag : m_summary_tags) {
        std::unique_ptr<std::vector<double> > outsum(new std::vector<double>);
        event.put(std::move(outsum), stag);
    }        
        
    for (auto jcmm : m_cmms) {
	std::string name = jcmm.asString();
	std::unique_ptr< channel_list > out_list(new channel_list);
	std::unique_ptr< channel_masks > out_masks(new channel_masks);
	event.put(std::move(out_list), name + "channels");
	event.put(std::move(out_masks), name + "masks");
    }
}

void FrameSaver::visit(art::Event & event)
{
    if (!m_frame) {
        save_empty(event);
        return;
    }

    if (m_digitize) {
        save_as_raw(event);
    }
    else {
        save_as_cooked(event);
    }

    save_summaries(event);

    save_cmms(event);

    m_frame = nullptr;		// done with stashed frame
}

bool FrameSaver::operator()(const WireCell::IFrame::pointer& inframe,
				  WireCell::IFrame::pointer& outframe)
{
    // set an IFrame based on last visited event.
    outframe = inframe;
    if (inframe) {
        if (m_frame) {
            std::cerr << "wclsFrameSaver: warning: dropping prior frame.  Fixme to handle queue of frames.\n";
        }
        // else {
        //     std::cerr << "wclsFrameSaver got frame\n";
        // }
        m_frame=inframe;
    }
    // else {
    //     std::cerr << "wclsFrameSaver sees EOS\n";
    // }
    return true;
}

// Local Variables:
// mode: c++
// c-basic-offset: 4
// End: