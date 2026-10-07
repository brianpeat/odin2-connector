// Odin 2: Preset Bridge handler (spec v0.1 draft). Serves the 260 factory presets that are baked into the binary.
#include "PluginProcessor.h"
#include "PresetBridgeCatalog.h"
#include <PresetBridge.h>

// Defined (non-inline) in gui/FactoryPresetBinaryMapping.h, which is only included by PatchBrowser.cpp.
std::pair<const char *, int> getFactoryPresetBinaryData(const std::string &p_preset);

namespace {
using juce::DynamicObject;
using juce::StringArray;
using juce::var;

const char *kRevision = "odin2-2.4.1-factory";
const char *kDemoPack = "odin-demo-organ";

bool isDemoRestricted(const char *category) { return juce::String(category) == "Organ"; }

// DEMO entitlement: "owned" if this marker file exists. A real plugin would check its own license/activation data.
bool demoPackOwned() {
	return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
	    .getChildFile("Application Support/Odin2/entitlements").getChildFile(kDemoPack).existsAsFile();
}

var errorResponse(const char *code, const juce::String &message) {
	auto *err = new DynamicObject();
	err->setProperty("code", code);
	err->setProperty("message", message);
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
	readPatch(juce::ValueTree::readFromStream(stream));
	m_connector_current_id = (juce::String("factory/") + kConnectorCatalog[index].category + "/" + kConnectorCatalog[index].name).toStdString();
	return true;
}

std::string OdinAudioProcessor::handleRequest(const std::string &requestJson) {
	const var req = juce::JSON::parse(juce::String(requestJson));
	if (!req.isObject())
		return juce::JSON::toString(errorResponse("bad_request", "request is not a JSON object"), true).toStdString();
	const juce::String op = req["op"].toString();
	auto *res = new DynamicObject();
	var out(res);
	res->setProperty("ok", true);

	if (op == "hello") {
		res->setProperty("connector", 1);
		auto *plugin = new DynamicObject();
		plugin->setProperty("name", "Odin 2");
		plugin->setProperty("id", "com.TheWaveWarden.Odin2");
		plugin->setProperty("version", "2.4.1");
		res->setProperty("plugin", var(plugin));
		juce::Array<var> ops;
		for (auto *o : {"hello", "list", "get", "load", "exportState", "current", "collections", "collection", "entitled"})
			ops.add(juce::String(o));
		res->setProperty("ops", var(ops));
		res->setProperty("revision", kRevision);
		auto *counts = new DynamicObject();
		counts->setProperty("presets", kConnectorCatalogSize);
		res->setProperty("counts", var(counts));
		auto *limits = new DynamicObject();
		limits->setProperty("pageMax", 500);
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
		res->setProperty("preset", presetRecord(i));
	} else if (op == "load" || op == "exportState") {
		const int i = indexForId(req["id"].toString());
		if (i < 0)
			return juce::JSON::toString(errorResponse("not_found", "no such preset"), true).toStdString();
		bool ok = true;
		juce::MemoryBlock exported;
		const bool isExport = (op == "exportState");
		runOnMessageThread([&] {
			if (!isExport) {
				ok = connectorLoadFactory(i);
				return;
			}
			// Export without disturbing the live sound: save, load the preset, capture state, restore.
			juce::MemoryBlock saved;
			getStateInformation(saved);
			const auto previousId = m_connector_current_id;
			ok = connectorLoadFactory(i);
			if (ok)
				getStateInformation(exported);
			setStateInformation(saved.getData(), (int)saved.getSize());
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
	} else if (op == "current") {
		if (!m_connector_current_id.empty())
			res->setProperty("id", juce::String(m_connector_current_id));
	} else {
		return juce::JSON::toString(errorResponse("unsupported", "unknown op: " + op), true).toStdString();
	}
	return juce::JSON::toString(out, true).toStdString();
}
