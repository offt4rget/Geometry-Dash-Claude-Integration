#include <Geode/Geode.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/EditorPauseLayer.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/async.hpp>

using namespace geode::prelude;

static const char* CHAT_SYSTEM =
	"You are Claude, chatting inside Geometry Dash through a mod. "
	"Keep answers short (a few sentences) because they show in a small popup.";

static const char* EDITOR_SYSTEM =
	"You are Claude, controlling the Geometry Dash level editor through a mod. "
	"Reply with ONLY a JSON object, no markdown, in this shape: "
	"{\"say\":\"short note to the player\",\"objects\":[{\"id\":1,\"x\":0,\"y\":0,\"rot\":0}]}. "
	"x and y are grid units (1 unit = 1 block = 30 game units), x increases to the right, y up, origin at the "
	"bottom-left of where the object batch starts. rot is degrees clockwise. "
	"Known object ids: 1 = basic block, 8 = spike, 39 = small spike, 35 = yellow pad, 36 = yellow orb. "
	"Use only these ids. Place at most 150 objects. If the request cannot be done with these objects, "
	"return an empty objects list and explain in say.";

class ClaudePopup : public geode::Popup {
protected:
	bool m_editor = false;
	TextInput* m_input = nullptr;
	SimpleTextArea* m_output = nullptr;
	async::TaskHolder<web::WebResponse> m_listener;

	bool init(bool editor) {
		if (!Popup::init(380.f, 250.f)) return false;
		m_editor = editor;
		this->setTitle(editor ? "Claude (Editor)" : "Claude");

		auto size = m_mainLayer->getContentSize();

		m_output = SimpleTextArea::create(
			editor ? "Describe what to build, e.g. a 5-block staircase with spikes at the end."
			       : "Ask me anything.",
			"chatFont.fnt", 0.6f, size.width - 40.f);
		m_output->setPosition({size.width / 2, size.height / 2 + 15.f});
		m_mainLayer->addChild(m_output);

		m_input = TextInput::create(size.width - 110.f, "Type here...");
		m_input->setPosition({size.width / 2 - 35.f, 35.f});
		m_mainLayer->addChild(m_input);

		auto sendSpr = ButtonSprite::create("Send");
		auto sendBtn = CCMenuItemSpriteExtra::create(sendSpr, this, menu_selector(ClaudePopup::onSend));
		sendBtn->setPosition({size.width - 45.f, 35.f});
		m_buttonMenu->addChild(sendBtn);

		return true;
	}

	void onSend(CCObject*) {
		auto key = Mod::get()->getSettingValue<std::string>("api-key");
		auto model = Mod::get()->getSettingValue<std::string>("model");
		if (key.empty()) {
			m_output->setText("Add your Anthropic API key in this mod's settings first.");
			return;
		}
		auto question = m_input->getString();
		if (question.empty()) return;

		// Tell Claude where the player is
		std::string context;
		if (auto pl = PlayLayer::get()) {
			context = fmt::format("[Player is in level \"{}\"]\n", std::string(pl->m_level->m_levelName));
		}

		matjson::Value msg;
		msg["role"] = "user";
		msg["content"] = context + question;

		matjson::Value body;
		body["model"] = model;
		body["max_tokens"] = 1024;
		body["system"] = m_editor ? EDITOR_SYSTEM : CHAT_SYSTEM;
		body["messages"] = std::vector<matjson::Value>{msg};

		m_output->setText("Thinking...");

		auto req = web::WebRequest();
		req.header("x-api-key", key);
		req.header("anthropic-version", "2023-06-01");
		req.bodyJSON(body);

		m_listener.spawn(
			req.post("https://api.anthropic.com/v1/messages"),
			[this](web::WebResponse res) {
				if (!res.ok()) {
					m_output->setText(fmt::format("Request failed (HTTP {}). Check your key and model name.", res.code()));
					return;
				}
				auto json = res.json();
				if (!json) {
					m_output->setText("Couldn't read the response.");
					return;
				}
				auto text = json.unwrap()["content"][0]["text"].asString().unwrapOr("");
				this->handleReply(text);
			}
		);
	}

	void handleReply(std::string const& text) {
		if (!m_editor) {
			m_output->setText(text);
			return;
		}

		auto a = text.find('{');
		auto b = text.rfind('}');
		if (a == std::string::npos || b == std::string::npos || b <= a) {
			m_output->setText("Claude didn't return usable commands:\n" + text);
			return;
		}
		auto parsed = matjson::parse(text.substr(a, b - a + 1));
		if (!parsed) {
			m_output->setText("Couldn't parse Claude's commands:\n" + text);
			return;
		}
		auto json = parsed.unwrap();
		std::string say = json["say"].asString().unwrapOr("");

		auto lel = LevelEditorLayer::get();
		if (!lel) {
			m_output->setText("Open this from inside the editor.");
			return;
		}

		// Start placing near the current camera position
		CCPoint base = -lel->m_objectLayer->getPosition();
		base.x = std::round(base.x / 30.f) * 30.f + 150.f;
		base.y = std::round(base.y / 30.f) * 30.f + 105.f;

		int placed = 0;
		auto arr = json["objects"].asArray();
		if (arr) {
			for (auto& o : arr.unwrap()) {
				if (placed >= 150) break;
				int id = o["id"].asInt().unwrapOr(0);
				if (id != 1 && id != 8 && id != 39 && id != 35 && id != 36) continue;
				float x = o["x"].asDouble().unwrapOr(0.0);
				float y = o["y"].asDouble().unwrapOr(0.0);
				float rot = o["rot"].asDouble().unwrapOr(0.0);
				auto obj = lel->createObject(id, base + CCPoint{x * 30.f, y * 30.f}, true);
				if (obj) {
					obj->setRotation(rot);
					placed++;
				}
			}
		}
		m_output->setText(fmt::format("{}\n\nPlaced {} object(s). Use undo if it's wrong.", say, placed));
	}

public:
	static ClaudePopup* create(bool editor) {
		auto ret = new ClaudePopup();
		if (ret && ret->init(editor)) {
			ret->autorelease();
			return ret;
		}
		CC_SAFE_DELETE(ret);
		return nullptr;
	}
};

class $modify(ClaudePauseLayer, PauseLayer) {
	void customSetup() {
		PauseLayer::customSetup();
		auto spr = ButtonSprite::create("Claude", "goldFont.fnt", "GJ_button_01.png", 0.8f);
		auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(ClaudePauseLayer::onClaude));
		auto menu = CCMenu::create();
		menu->addChild(btn);
		menu->setPosition({70.f, 40.f});
		this->addChild(menu);
	}

	void onClaude(CCObject*) {
		ClaudePopup::create(false)->show();
	}
};

class $modify(ClaudeEditorPauseLayer, EditorPauseLayer) {
	bool init(LevelEditorLayer* lel) {
		if (!EditorPauseLayer::init(lel)) return false;
		auto spr = ButtonSprite::create("Claude", "goldFont.fnt", "GJ_button_01.png", 0.8f);
		auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(ClaudeEditorPauseLayer::onClaude));
		auto menu = CCMenu::create();
		menu->addChild(btn);
		menu->setPosition({70.f, 40.f});
		this->addChild(menu);
		return true;
	}

	void onClaude(CCObject*) {
		ClaudePopup::create(true)->show();
	}
};
