// Odin 2: Preset Bridge handler (spec v0.2 draft). Serves the 260 factory presets that are baked into the binary.
#include "PluginProcessor.h"
#include "PresetBridgeCatalog.h"
#include <PresetBridge.h>
#include <cstdlib>

// Defined (non-inline) in gui/FactoryPresetBinaryMapping.h, which is only included by PatchBrowser.cpp.
std::pair<const char *, int> getFactoryPresetBinaryData(const std::string &p_preset);

namespace {
using juce::DynamicObject;
using juce::StringArray;
using juce::var;

const char *kRevision = "odin2-2.4.1-factory";
const char *kDemoPack = "odin-demo-organ";

// ---- Access level and throttling (spec 3.1 and 3.2) --------------------------------------------------------------------
// Odin 2 is open source, so by default it shares everything ("open", no throttle). To try the lower levels, set
// PRESETBRIDGE_ACCESS=catalog|audition|insight|open (and optionally PRESETBRIDGE_LOADS_PER_MINUTE=N) before the plugin loads.
struct BridgeConfig {
	presetbridge::Access access = presetbridge::Access::open;
	int loadsPerMinute = 0;
	BridgeConfig() {
		if (const char *a = std::getenv("PRESETBRIDGE_ACCESS"))
			access = presetbridge::accessFromName(a, presetbridge::Access::open);
		loadsPerMinute = (access == presetbridge::Access::audition || access == presetbridge::Access::insight) ? 10 : 0;
		if (const char *n = std::getenv("PRESETBRIDGE_LOADS_PER_MINUTE"))
			loadsPerMinute = std::atoi(n);
	}
};
BridgeConfig &bridgeConfig() {
	static BridgeConfig c;
	return c;
}
presetbridge::Throttle &loadThrottle() {
	static presetbridge::Throttle t(bridgeConfig().loadsPerMinute);
	return t;
}
bool opAllowed(const juce::String &op) {
	using presetbridge::Access;
	using presetbridge::atLeast;
	const Access a = bridgeConfig().access;
	if (op == "hello" || op == "list" || op == "get" || op == "collections" || op == "collection")
		return true;
	if (op == "load" || op == "current" || op == "entitled" || op == "events")
		return atLeast(a, Access::audition);
	if (op == "params")
		return atLeast(a, Access::insight);
	if (op == "exportState")
		return atLeast(a, Access::open);
	return false;
}

// ---- Roles: Odin 2's own ids -> the spec's shared vocabulary (spec 6, roles) ----------------------------------------------------
// Sources in the mod matrix (ModMatrix.cpp setModSource). Anything without a shared name keeps an Odin-specific one.
std::string sourceRole(int s) {
	switch (s) {
	case 100: case 101: case 102: return "vendor.odin.osc" + std::to_string(s - 99);
	case 110: case 111: return "vendor.odin.filter" + std::to_string(s - 109);
	case 200: return "env.amp";
	case 201: return "env.filter";
	case 202: return "env.mod";
	case 203: return "env.global";
	case 300: case 301: case 302: return "lfo." + std::to_string(s - 299);
	case 303: return "lfo.4";
	case 400: return "macro.1";   // XY pad X
	case 401: return "macro.2";   // XY pad Y
	case 402: return "modwheel";
	case 403: return "pitchbend";
	case 404: return "vendor.odin.key";
	case 405: return "velocity";
	case 407: return "vendor.odin.breath";
	case 409: return "aftertouch";   // channel pressure
	case 420: return "vendor.odin.unison_position";
	case 430: return "vendor.odin.arp_mod_1";
	case 435: return "vendor.odin.arp_mod_2";
	case 440: return "sustain";
	case 450: return "vendor.odin.soft_pedal";
	case 999: return "vendor.odin.random";
	case 1000: return "constant";
	default: return "vendor.odin.source" + std::to_string(s);
	}
}
// Destinations in the mod matrix (ModMatrix.cpp setModDestination1/2).
std::string destRole(int d) {
	using std::to_string;
	if (d >= 2 && d < 300) {   // oscillators: tens digit pair = parameter, hundreds digit = oscillator
		const int osc = d / 100 + 1, base = d % 100;
		const std::string o = "osc." + to_string(osc) + ".";
		switch (base) {
		case 2: case 3: return o + "pitch";
		case 4: return o + "level";
		case 10: return o + "pulsewidth";
		case 20: return o + "position";
		case 21: return o + "detune";
		case 22: return o + "spread";
		case 30: return "vendor.odin." + o + "x";
		case 31: return "vendor.odin." + o + "y";
		case 40: return "vendor.odin." + o + "arp_speed";
		case 50: return o + "fm.amount";
		case 51: return "vendor.odin." + o + "carrier_ratio";
		case 52: return "vendor.odin." + o + "modulator_ratio";
		case 60: return o + "lowpass";
		case 61: return o + "highpass";
		}
	}
	if (d >= 301 && d < 600) {   // filters 1 to 3
		const std::string f = "filter." + to_string(d / 100 - 2) + ".";
		switch (d % 100) {
		case 1: return f + "cutoff";
		case 2: return f + "resonance";
		case 3: return f + "gain";
		case 4: return f + "envamount";
		case 5: return f + "velocity";
		case 6: return f + "keytrack";
		case 7: return f + "drive";
		case 10: return "vendor.odin." + f + "sem_transition";
		case 20: return "vendor.odin." + f + "formant_transition";
		case 30: return "vendor.odin." + f + "ringmod";
		}
	}
	if (d >= 601 && d <= 634) {   // envelopes: tens digit = envelope (amp, filter, mod, global), ones digit = stage
		static const char *env[] = {"amp", "filter", "mod", "global"};
		static const char *stage[] = {"", "attack", "decay", "sustain", "release"};
		const int e = (d - 600) / 10, st = d % 10;
		if (e >= 0 && e < 4 && st >= 1 && st <= 4) return std::string("env.") + env[e] + "." + stage[st];
	}
	switch (d) {
	case 651: return "lfo.1.rate"; case 661: return "lfo.2.rate"; case 671: return "lfo.3.rate"; case 681: return "lfo.4.rate";
	case 701: return "fx.delay.time"; case 702: return "fx.delay.feedback"; case 703: return "vendor.odin.delay.hp_freq";
	case 704: return "vendor.odin.delay.dry"; case 705: return "fx.delay.mix";
	case 751: return "fx.phaser.rate"; case 752: return "fx.phaser.depth"; case 753: return "fx.phaser.freq";
	case 754: return "fx.phaser.feedback"; case 755: return "fx.phaser.mix";
	case 801: return "fx.flanger.rate"; case 802: return "fx.flanger.depth"; case 803: return "fx.flanger.feedback"; case 804: return "fx.flanger.mix";
	case 851: return "fx.chorus.rate"; case 852: return "fx.chorus.depth"; case 853: return "fx.chorus.feedback"; case 854: return "fx.chorus.mix";
	case 900: return "amp.level"; case 901: return "pan"; case 902: return "vendor.odin.amp_velocity";
	case 950: return "drive"; case 951: return "fx.distortion.mix";
	case 970: return "vendor.odin.arp_speed"; case 973: return "vendor.odin.arp_gate";
	case 980: return "vendor.odin.xy_x"; case 982: return "vendor.odin.xy_y";
	case 999: return "vendor.odin.glide"; case 1000: return "output.level";
	}
	return "vendor.odin.destination" + to_string(d);
}
// Odin 2 parameter id -> shared role ("" when none fits).
std::string paramRole(const juce::String &id) {
	using std::to_string;
	const std::string s = id.toStdString();
	auto digitAt = [&](size_t i) { return i < s.size() && s[i] >= '0' && s[i] <= '9' ? s[i] - '0' : -1; };
	auto tail = [&](size_t from) { return s.size() > from ? s.substr(from) : std::string(); };
	if (s.rfind("fil", 0) == 0 && digitAt(3) >= 1 && digitAt(3) <= 3 && s[4] == '_') {
		static const std::pair<const char *, const char *> m[] = {{"freq", "cutoff"}, {"res", "resonance"}, {"gain", "gain"}, {"env", "envamount"},
		                                                          {"vel", "velocity"}, {"kbd", "keytrack"}, {"saturation", "drive"}};
		for (auto &p : m) if (tail(5) == p.first) return "filter." + to_string(digitAt(3)) + "." + p.second;
	}
	if (s.rfind("env", 0) == 0 && digitAt(3) >= 1 && digitAt(3) <= 4 && s[4] == '_') {
		static const char *env[] = {"amp", "filter", "mod", "global"};
		const std::string st = tail(5);
		if (st == "attack" || st == "decay" || st == "sustain" || st == "release") return std::string("env.") + env[digitAt(3) - 1] + "." + st;
	}
	if (s.rfind("osc", 0) == 0 && digitAt(3) >= 1 && digitAt(3) <= 3 && s[4] == '_') {
		static const std::pair<const char *, const char *> m[] = {{"vol", "level"}, {"detune", "detune"}, {"spread", "spread"}, {"position", "position"},
		                                                          {"pulsewidth", "pulsewidth"}, {"oct", "octave"}, {"semi", "semitone"}, {"fine", "fine"}};
		for (auto &p : m) if (tail(5) == p.first) return "osc." + to_string(digitAt(3)) + "." + p.second;
	}
	if (s.rfind("lfo", 0) == 0 && digitAt(3) >= 1 && digitAt(3) <= 4 && tail(4) == "_freq") return "lfo." + to_string(digitAt(3)) + ".rate";
	static const std::pair<const char *, const char *> fixed[] = {
	    {"amp_gain", "amp.level"}, {"amp_pan", "pan"}, {"master", "output.level"}, {"xy_x", "macro.1"}, {"xy_y", "macro.2"},
	    {"modwheel", "performance.modwheel"}, {"pitchbend", "performance.pitchbend"},
	    {"delay_time", "fx.delay.time"}, {"delay_feedback", "fx.delay.feedback"}, {"delay_wet", "fx.delay.mix"},
	    {"phaser_rate", "fx.phaser.rate"}, {"phaser_amount", "fx.phaser.depth"}, {"phaser_freq", "fx.phaser.freq"}, {"phaser_feedback", "fx.phaser.feedback"}, {"phaser_drywet", "fx.phaser.mix"},
	    {"flanger_freq", "fx.flanger.rate"}, {"flanger_amount", "fx.flanger.depth"}, {"flanger_feedback", "fx.flanger.feedback"}, {"flanger_drywet", "fx.flanger.mix"},
	    {"chorus_freq", "fx.chorus.rate"}, {"chorus_amount", "fx.chorus.depth"}, {"chorus_feedback", "fx.chorus.feedback"}, {"chorus_drywet", "fx.chorus.mix"}};
	for (auto &p : fixed) if (s == p.first) return p.second;
	return {};
}

bool isDemoRestricted(const char *category) { return juce::String(category) == "Organ"; }

// DEMO entitlement: "owned" if this marker file exists. A real plugin would check its own license/activation data.
bool demoPackOwned() {
	return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
	    .getChildFile("Application Support/Odin2/entitlements").getChildFile(kDemoPack).existsAsFile();
}

var errorResponse(const char *code, const juce::String &message, int retryAfterMs = 0) {
	auto *err = new DynamicObject();
	err->setProperty("code", code);
	err->setProperty("message", message);
	if (retryAfterMs > 0)
		err->setProperty("retryAfterMs", retryAfterMs);
	auto *r = new DynamicObject();
	r->setProperty("ok", false);
	r->setProperty("error", var(err));
	return var(r);
}

var presetRecord(int index) {
	const auto &e = kConnectorCatalog[index];
	auto *r = new DynamicObject();
	r->setProperty("id", juce::String("factory/") + e.category + "/" + e.name);
	r->setProperty("name", juce::String(juce::CharPointer_UTF8(e.name)));
	r->setProperty("origin", "factory");
	r->setProperty("available", true);
	juce::Array<var> path;
	path.add(juce::String(juce::CharPointer_UTF8(e.category)));
	juce::Array<var> cats;
	cats.add(var(path));
	r->setProperty("categories", var(cats));
	auto *rights = new DynamicObject();
	if (isDemoRestricted(e.category)) {
		// DEMO ONLY: pretends the Organ category is a paid pack, to exercise rights + entitlement.
		rights->setProperty("level", "restricted");
		rights->setProperty("pack", kDemoPack);
		rights->setProperty("terms", "https://example.com/odin-demo-pack");
		auto *pack = new DynamicObject();
		pack->setProperty("id", kDemoPack);
		pack->setProperty("name", "Demo Organ Pack (fake)");
		r->setProperty("pack", var(pack));
		// DEMO ONLY: a per-preset policy (spec 3.4): this pack may not be captured out of the plugin or shared.
		auto *policy = new DynamicObject();
		policy->setProperty("capture", "deny");
		policy->setProperty("share", "deny");
		r->setProperty("policy", var(policy));
	} else {
		rights->setProperty("level", "free");  // GPLv3 plugin, factory patches ship with the source
	}
	r->setProperty("rights", var(rights));
	const juce::String cat(e.category);
	if (cat == "Arps & Sequences" || cat == "Atmospheres") {
		auto *aud = new DynamicObject();
		aud->setProperty("kind", cat == "Atmospheres" ? "pad" : "sequence");
		aud->setProperty("tempoSync", cat != "Atmospheres");
		aud->setProperty("tailSeconds", cat == "Atmospheres" ? 8 : 2);
		r->setProperty("audition", var(aud));
	}
	return var(r);
}

int indexForId(const juce::String &id) {
	for (int i = 0; i < kConnectorCatalogSize; ++i)
		if (juce::String("factory/") + kConnectorCatalog[i].category + "/" + kConnectorCatalog[i].name == id)
			return i;
	return -1;
}
}  // namespace

// Controller routing for one factory preset (spec 5.6): the mod-matrix rows, read from the preset the way Odin 2 itself reads it.
// Row r: source_row_r -> dest_1_row_r with amount_0_row_r and dest_2_row_r with amount_1_row_r; scale_row_r / amount_2_row_r scale the
// result. Odin applies source * a * |a|, so the share of the target's range moved at full travel is a * |a|.
static var controlsForPreset(OdinAudioProcessor &proc, int index) {
	juce::Array<var> out;
	const auto data = getFactoryPresetBinaryData(kConnectorCatalog[index].name);
	if (data.first == nullptr) return var(out);
	juce::MemoryInputStream stream(data.first, (size_t)data.second, false);
	auto tree = juce::ValueTree::readFromStream(stream).createCopy();
	proc.migratePatchForBridge(tree);   // older presets store some values as text; this makes them numbers, as Odin does on load
	// Fixed routings: wired into the instrument, not in the mod matrix. Stored as plain parameters, so a preset can still set their amounts.
	// Marked "fixed": true so a host can tell them from routings the preset author drew in the matrix.
	auto addFixed = [&](const char *source, const juce::String &target, double amount) {
		auto *c = new DynamicObject();
		c->setProperty("source", juce::String(source));
		c->setProperty("target", target);
		c->setProperty("amount", amount);
		c->setProperty("polarity", amount >= 0 ? "up" : "down");
		c->setProperty("fixed", true);
		out.add(var(c));
	};
	for (int i = 0; i < tree.getNumChildren(); ++i) {
		const auto param = tree.getChild(i);
		if (!param.hasType("PARAM")) continue;
		const juce::String id = param["id"].toString();
		const double v = (double)param["value"];
		if (std::abs(v) < 0.0005) continue;
		if (id == "amp_velocity") addFixed("velocity", "amp.level", v);
		else if (id.length() == 8 && id.startsWith("fil") && id.endsWith("_vel")) addFixed("velocity", juce::String("filter.") + id[3] + ".cutoff", v);
		else if (id.length() == 8 && id.startsWith("fil") && id.endsWith("_kbd")) addFixed("vendor.odin.key", juce::String("filter.") + id[3] + ".cutoff", v);
		else if (id.length() == 8 && id.startsWith("fil") && id.endsWith("_env")) addFixed("env.filter", juce::String("filter.") + id[3] + ".cutoff", v);
	}
	{
		const auto misc = tree.getChildWithName("misc");
		const double semis = misc.isValid() ? (double)misc["pitchbend_amount"] : 0.0;
		if (semis > 0.0) {
			auto *c = new DynamicObject();
			c->setProperty("source", "pitchbend");
			c->setProperty("target", "pitch");
			c->setProperty("fixed", true);
			c->setProperty("vendor.odin.semitones", semis);
			out.add(var(c));
		}
	}
	const auto mod = tree.getChildWithName("mod");
	if (!mod.isValid()) return var(out);
	auto num = [&](const juce::String &key) { return (double)mod[key]; };
	for (int r = 0; r < 9; ++r) {
		const int source = (int)num("source_row_" + juce::String(r));
		if (source == 0) continue;
		const int scale = (int)num("scale_row_" + juce::String(r));
		const double scaleAmount = num("amount_2_row_" + juce::String(r));
		for (int d = 1; d <= 2; ++d) {
			const int dest = (int)num("dest_" + juce::String(d) + "_row_" + juce::String(r));
			const double a = num("amount_" + juce::String(d - 1) + "_row_" + juce::String(r));
			if (dest == 0 || std::abs(a) < 0.0005) continue;
			const double effective = juce::jlimit(-1.0, 1.0, a * std::abs(a));
			auto *c = new DynamicObject();
			c->setProperty("source", juce::String(sourceRole(source)));
			c->setProperty("target", juce::String(destRole(dest)));
			c->setProperty("amount", effective);
			c->setProperty("polarity", effective >= 0 ? "up" : "down");
			c->setProperty("vendor.odin.row", r + 1);
			if (scale != 0 && std::abs(scaleAmount) >= 0.0005) {
				c->setProperty("vendor.odin.scaledBy", juce::String(sourceRole(scale)));
				c->setProperty("vendor.odin.scaleAmount", scaleAmount);
			}
			out.add(var(c));
			// A source that scales another (mod wheel scaling an oscillator's modulation of the filter) is a routing of its own: without
			// the wheel the modulation is silent. Listed as a second entry with the scaler as `source`, so "what does the wheel do?" finds it.
			if (scale != 0 && std::abs(scaleAmount) >= 0.0005) {
				auto *s2 = new DynamicObject();
				s2->setProperty("source", juce::String(sourceRole(scale)));
				s2->setProperty("target", juce::String(destRole(dest)));
				s2->setProperty("amount", effective);
				s2->setProperty("polarity", effective >= 0 ? "up" : "down");
				s2->setProperty("mode", "scales");
				s2->setProperty("scales", juce::String(sourceRole(source)));
				s2->setProperty("vendor.odin.row", r + 1);
				out.add(var(s2));
			}
		}
	}
	return var(out);
}

// Records that a patch was loaded, for the pull-style `events` op. A load the bridge made itself already knows its id; a load made
// from Odin's own browser is matched to the catalog by patch name when that name is unique.
void OdinAudioProcessor::connectorPatchLoaded(const juce::ValueTree &patch) {
	if (m_connector_quiet) return;
	std::string id = m_connector_loading ? m_connector_current_id : std::string();
	const juce::String name = patch.getChildWithName("misc")["patch_name"].toString();
	if (id.empty() && name.isNotEmpty()) {
		int matches = 0, found = -1;
		for (int i = 0; i < kConnectorCatalogSize; ++i)
			if (name == juce::String(juce::CharPointer_UTF8(kConnectorCatalog[i].name))) { ++matches; found = i; }
		if (matches == 1)
			id = (juce::String("factory/") + kConnectorCatalog[found].category + "/" + kConnectorCatalog[found].name).toStdString();
	}
	std::lock_guard<std::mutex> g(m_connector_events_mutex);
	m_connector_events.push_back({++m_connector_event_seq, "presetChanged", id, name.toStdString()});
	while (m_connector_events.size() > 256) m_connector_events.pop_front();
}

// Runs fn on the JUCE message thread (immediately if already there) and waits for it.
static void runOnMessageThread(std::function<void()> fn) {
	if (juce::MessageManager::getInstance()->isThisTheMessageThread()) {
		fn();
		return;
	}
	struct Ctx { std::function<void()> fn; } ctx{std::move(fn)};
	juce::MessageManager::getInstance()->callFunctionOnMessageThread(
	    [](void *c) -> void * {
		    static_cast<Ctx *>(c)->fn();
		    return nullptr;
	    },
	    &ctx);
}

bool OdinAudioProcessor::connectorLoadFactory(int index) {
	const auto data = getFactoryPresetBinaryData(kConnectorCatalog[index].name);
	if (data.first == nullptr)
		return false;
	juce::MemoryInputStream stream(data.first, (size_t)data.second, false);
	m_connector_current_id = (juce::String("factory/") + kConnectorCatalog[index].category + "/" + kConnectorCatalog[index].name).toStdString();
	m_connector_loading = true;
	readPatch(juce::ValueTree::readFromStream(stream));
	m_connector_loading = false;
	return true;
}

std::string OdinAudioProcessor::handleRequest(const std::string &requestJson) {
	const var req = juce::JSON::parse(juce::String(requestJson));
	if (!req.isObject())
		return juce::JSON::toString(errorResponse("bad_request", "request is not a JSON object"), true).toStdString();
	if (!req["op"].isString() || req["op"].toString().isEmpty())
		return juce::JSON::toString(errorResponse("bad_request", "request needs a string \"op\""), true).toStdString();
	const juce::String op = req["op"].toString();
	auto *res = new DynamicObject();
	var out(res);
	res->setProperty("ok", true);

	if (!opAllowed(op))
		return juce::JSON::toString(errorResponse("unsupported", "not offered at access level " + juce::String(presetbridge::accessName(bridgeConfig().access)) + ": " + op), true).toStdString();

	if (op == "hello") {
		res->setProperty("connector", 1);
		auto *plugin = new DynamicObject();
		plugin->setProperty("name", "Odin 2");
		plugin->setProperty("id", "com.TheWaveWarden.Odin2");
		plugin->setProperty("version", "2.4.1");
		plugin->setProperty("kind", "instrument");
		auto *au = new DynamicObject();
		au->setProperty("type", "aumu");
		au->setProperty("subtype", "ODIN");
		au->setProperty("manufacturer", "WAWA");
		auto *identity = new DynamicObject();
		identity->setProperty("au", var(au));
		plugin->setProperty("identity", var(identity));
		res->setProperty("plugin", var(plugin));
		res->setProperty("access", presetbridge::accessName(bridgeConfig().access));
		juce::Array<var> ops;
		for (auto *o : {"hello", "list", "get", "load", "exportState", "current", "collections", "collection", "entitled", "events", "params"})
			if (opAllowed(o))
				ops.add(juce::String(o));
		res->setProperty("ops", var(ops));
		res->setProperty("revision", kRevision);
		auto *counts = new DynamicObject();
		counts->setProperty("presets", kConnectorCatalogSize);
		res->setProperty("counts", var(counts));
		auto *limits = new DynamicObject();
		limits->setProperty("pageMax", 500);
		limits->setProperty("timeoutMs", 30000);
		limits->setProperty("concurrent", 1);
		if (bridgeConfig().loadsPerMinute > 0)
			limits->setProperty("loadsPerMinute", bridgeConfig().loadsPerMinute);
		res->setProperty("limits", var(limits));
	} else if (op == "list") {
		const int cursor = juce::jmax(0, (int)req["cursor"]);
		const int limit = juce::jlimit(1, 500, req.hasProperty("limit") ? (int)req["limit"] : 500);
		juce::Array<var> presets;
		int i = cursor;
		for (; i < kConnectorCatalogSize && presets.size() < limit; ++i)
			presets.add(presetRecord(i));
		res->setProperty("presets", var(presets));
		if (i < kConnectorCatalogSize)
			res->setProperty("next", juce::String(i));
		res->setProperty("revision", kRevision);
	} else if (op == "get") {
		const int i = indexForId(req["id"].toString());
		if (i < 0)
			return juce::JSON::toString(errorResponse("not_found", "no such preset"), true).toStdString();
		var record = presetRecord(i);
		if (presetbridge::atLeast(bridgeConfig().access, presetbridge::Access::insight)) {
			// Controller routing is part of the full record only (it needs the preset parsed), not of every row of `list`.
			if (auto *obj = record.getDynamicObject())
				obj->setProperty("controls", controlsForPreset(*this, i));
		}
		res->setProperty("preset", record);
	} else if (op == "load" || op == "exportState") {
		const int i = indexForId(req["id"].toString());
		if (i < 0)
			return juce::JSON::toString(errorResponse("not_found", "no such preset"), true).toStdString();
		const bool isExport = (op == "exportState");
		if (isExport && isDemoRestricted(kConnectorCatalog[i].category))
			return juce::JSON::toString(errorResponse("denied", "this preset's policy does not allow capture"), true).toStdString();
		if (!isExport) {
			// The plugin, not the host, enforces the load limit (spec 3.2).
			const int wait = loadThrottle().tryUse();
			if (wait > 0)
				return juce::JSON::toString(errorResponse("rate_limited", "too many loads; slow down", wait), true).toStdString();
		}
		bool ok = true;
		juce::MemoryBlock exported;
		runOnMessageThread([&] {
			if (!isExport) {
				ok = connectorLoadFactory(i);
				return;
			}
			// Export without disturbing the live sound: save, load the preset, capture state, restore.
			juce::MemoryBlock saved;
			getStateInformation(saved);
			const auto previousId = m_connector_current_id;
			m_connector_quiet = true;   // borrowing the engine: do not report these loads as preset changes
			ok = connectorLoadFactory(i);
			if (ok)
				getStateInformation(exported);
			setStateInformation(saved.getData(), (int)saved.getSize());
			m_connector_quiet = false;
			m_connector_current_id = previousId;
		});
		if (!ok)
			return juce::JSON::toString(errorResponse("failed", "could not load preset"), true).toStdString();
		if (isExport) {
			res->setProperty("stateKind", "au.fullState/juce-getStateInformation");
			res->setProperty("state", juce::Base64::toBase64(exported.getData(), exported.getSize()));
		}
	} else if (op == "collections" || op == "collection") {
		// Factory categories double as collections.
		juce::StringArray cats;
		for (int i = 0; i < kConnectorCatalogSize; ++i)
			cats.addIfNotAlreadyThere(juce::String(kConnectorCatalog[i].category));
		if (op == "collections") {
			juce::Array<var> list;
			for (auto &c : cats) {
				auto *o = new DynamicObject();
				o->setProperty("id", "category/" + c);
				o->setProperty("name", c);
				o->setProperty("kind", "factory");
				list.add(var(o));
			}
			res->setProperty("collections", var(list));
		} else {
			const auto id = req["id"].toString();
			if (!id.startsWith("category/") || !cats.contains(id.fromFirstOccurrenceOf("category/", false, false)))
				return juce::JSON::toString(errorResponse("not_found", "no such collection"), true).toStdString();
			const auto name = id.fromFirstOccurrenceOf("category/", false, false);
			juce::Array<var> ids;
			for (int i = 0; i < kConnectorCatalogSize; ++i)
				if (juce::String(kConnectorCatalog[i].category) == name)
					ids.add(presetRecord(i)["id"]);
			auto *o = new DynamicObject();
			o->setProperty("id", id);
			o->setProperty("name", name);
			o->setProperty("kind", "factory");
			o->setProperty("presetIds", var(ids));
			res->setProperty("collection", var(o));
		}
	} else if (op == "entitled") {
		// Accepts "packs":[...] or "ids":[...]; answers per item. Local answer only, never sent anywhere.
		auto *answers = new DynamicObject();
		auto answerFor = [&](const juce::String &pack) { return juce::String(pack == kDemoPack ? (demoPackOwned() ? "owned" : "not_owned") : "unknown"); };
		if (auto *packs = req["packs"].getArray())
			for (auto &p : *packs) answers->setProperty(p.toString(), answerFor(p.toString()));
		if (auto *ids = req["ids"].getArray())
			for (auto &id : *ids) {
				const int i = indexForId(id.toString());
				answers->setProperty(id.toString(), i < 0 ? juce::String("unknown") : isDemoRestricted(kConnectorCatalog[i].category) ? answerFor(kDemoPack) : juce::String("owned"));
			}
		res->setProperty("entitled", var(answers));
	} else if (op == "events") {
		// Pull model (spec 6): `since` is the sequence number of the last event the host saw; omit it the first time.
		const long long since = req.hasProperty("since") ? (long long)req["since"].toString().getLargeIntValue() : -1;
		juce::Array<var> events;
		long long last = 0;
		{
			std::lock_guard<std::mutex> g(m_connector_events_mutex);
			last = m_connector_event_seq;
			if (since >= 0)   // the first call only establishes the cursor, so it does not replay old history
				for (const auto &e : m_connector_events)
					if (e.seq > since && events.size() < 100) {
						auto *o = new DynamicObject();
						o->setProperty("type", juce::String(e.type));
						if (!e.id.empty()) o->setProperty("id", juce::String(juce::CharPointer_UTF8(e.id.c_str())));
						if (!e.name.empty()) o->setProperty("name", juce::String(juce::CharPointer_UTF8(e.name.c_str())));
						events.add(var(o));
					}
		}
		res->setProperty("events", var(events));
		res->setProperty("next", juce::String(last));
	} else if (op == "params") {
		juce::Array<var> parameters;
		for (auto *p : getParameters()) {
			auto *pid = dynamic_cast<juce::AudioProcessorParameterWithID *>(p);
			if (!pid) continue;
			auto *o = new DynamicObject();
			o->setProperty("id", pid->paramID);
			o->setProperty("name", pid->getName(100));
			o->setProperty("section", pid->paramID.upToFirstOccurrenceOf("_", false, false));
			const auto role = paramRole(pid->paramID);
			if (!role.empty()) o->setProperty("role", juce::String(role));
			if (pid->getLabel().isNotEmpty()) o->setProperty("unit", pid->getLabel());
			if (auto *rp = dynamic_cast<juce::RangedAudioParameter *>(p)) {
				const auto range = rp->getNormalisableRange();
				o->setProperty("min", range.start);
				o->setProperty("max", range.end);
				o->setProperty("default", rp->convertFrom0to1(rp->getDefaultValue()));
				if (range.interval > 0) o->setProperty("step", range.interval);
			}
			parameters.add(var(o));
		}
		res->setProperty("parameters", var(parameters));
		juce::Array<var> macros;
		macros.add("xy_x");
		macros.add("xy_y");
		res->setProperty("macros", var(macros));
	} else if (op == "current") {
		if (!m_connector_current_id.empty())
			res->setProperty("id", juce::String(m_connector_current_id));
	} else {
		return juce::JSON::toString(errorResponse("unsupported", "unknown op: " + op), true).toStdString();
	}
	return juce::JSON::toString(out, true).toStdString();
}
