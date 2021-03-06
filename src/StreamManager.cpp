#include "StreamManager.h"
#include "Config.h"
#include "SystemInterface.h"
#include "Logger.h"

#include <fstream>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/assign/list_inserter.hpp>
#include <boost/assign/list_of.hpp>
#include <boost/assign/std/vector.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

#include "opencv2/highgui/highgui.hpp"

using namespace boost::assign;

namespace hs {

StreamManager::StreamManager(StreamPtr stream, clever_bot::botPtr bot) {
	this->stream = stream;
	this->bot = bot;
	cp = CommandProcessorPtr(new CommandProcessor(StreamManagerPtr(this)));
	db = DatabasePtr(new Database(Config::getConfig().get<std::string>("config.paths.recognition_data_path")));
	recognizer = RecognizerPtr(new Recognizer(db, Config::getConfig().get<std::string>("config.stream.streamer")));

	//zeroing out
	shouldUpdateDeck = false;
	param_debug_level = 0;
	passedFrames = PASSED_FRAMES_THRESHOLD;
	currentCard.second = -1;
	currentCard.second = 0;
	winsLosses = std::make_pair<int, int>(0,0);
	internalState = 0;
	deckInfo.state = 0;
	gameInfo.state = 0;
	drawInfo.state = 0;
	deckInfo.clear();

	//default state
	if (Config::getConfig().get<bool>("config.debugging.enabled", false)) param_debug_level = Config::getConfig().get<int>("config.debugging.debug_level");
	enable(internalState, INTERNAL_ENABLE_ALL);

	enable(deckInfo.state, RECOGNIZER_DRAFT_CLASS_PICK);
	enable(deckInfo.state, RECOGNIZER_DRAFT_CARD_PICK);

	enable(gameInfo.state, RECOGNIZER_GAME_CLASS_SHOW);
	enable(gameInfo.state, RECOGNIZER_GAME_END);

	if (internalState & INTERNAL_DRAWHANDLING) {
		enable(drawInfo.state, RECOGNIZER_GAME_DRAW);
	}

	loadState();

	api.submitDeckFormat = Config::getConfig().get<std::string>("config.site_interfacing.submit_deck");
	api.drawCardFormat = Config::getConfig().get<std::string>("config.site_interfacing.draw_card");
	api.resetDrawsFormat = Config::getConfig().get<std::string>("config.site_interfacing.reset_draws");

	sName = Config::getConfig().get<std::string>("config.stream.streamer_name");
	numThreads = Config::getConfig().get<int>("config.image_recognition.threads");

	if (numThreads <= 0) {
		numThreads = boost::thread::hardware_concurrency();
		if (numThreads <= 0) {
			HS_ERROR << "Unknown amount of cores, setting to 1" << std::endl;
			numThreads = 1;
		}
	}
	HS_INFO << "Using " << numThreads << " threads" << std::endl;
	this->stream->setCopyOnRead(numThreads > 1);
}

StreamManager::~StreamManager() {
	cp.reset();
}

void StreamManager::loadState() {
	boost::property_tree::ptree state;
    std::ifstream stateFile((boost::format(STATE_PATH_FORMAT) % Config::getConfig().get<std::string>("config.stream.streamer")).str());
    if (stateFile.fail()) {
    	HS_INFO << "no state to load, using default values" << std::endl;
    } else {
        boost::property_tree::read_xml(stateFile, state);
        deckInfo.msg = state.get<decltype(deckInfo.msg)>("state.deck_msg", deckInfo.msg);
        internalState = state.get<decltype(internalState)>("state.internal_state", internalState);
        winsLosses.first = state.get<decltype(winsLosses.first)>("state.current_wins", winsLosses.first);
        winsLosses.second = state.get<decltype(winsLosses.second)>("state.current_losses", winsLosses.second);

        std::string deckRep;
        deckRep = state.get<decltype(deckRep)>("state.data.deck", deckRep);
        if (!deckRep.empty()) {
        	deck.fillFromInternalRepresentation(db, deckRep);
        	shouldUpdateDeck = true;
        }

        HS_INFO << "state loaded" << std::endl;
    }
}

void StreamManager::saveState() {
	HS_INFO << "attempting to save state" << std::endl;
	std::string statePath = (boost::format(STATE_PATH_FORMAT) % Config::getConfig().get<std::string>("config.stream.streamer")).str();
	std::ifstream stateFile(statePath);
	boost::property_tree::ptree state;

    state.put("state.deck_msg", deckInfo.msg);
    state.put("state.internal_state", internalState);
    state.put("state.current_wins", winsLosses.first);
    state.put("state.current_losses", winsLosses.second);
    state.put("state.data.deck", deck.createInternalRepresentation());

    boost::property_tree::xml_writer_settings<char> settings('\t', 1);
    write_xml(statePath, state, std::locale(""), settings);
    HS_INFO << "state saved" << std::endl;
}

void StreamManager::setStream(StreamPtr stream) {
	this->stream = stream;
}

void StreamManager::startAsyn() {
	if (Config::getConfig().get<bool>("config.debugging.enabled", false)) {
		stream->setStreamIndex(Config::getConfig().get<int>("config.debugging.stream_index"));
		stream->setFramePos(Config::getConfig().get<int>("config.debugging.stream_pos"));
	}

	for (int i = 0; i < numThreads; i++) {
		processingThreads.create_thread(boost::bind(&StreamManager::run, this));
	}
}

void StreamManager::wait() {
	processingThreads.join_all();
}

void StreamManager::run() {
	cv::Mat image;

	HS_INFO << "Started thread" << std::endl;

	bool running = true;
	while (running) {
		const bool validImage = stream->read(image);
		if (!validImage) break;

		if (param_debug_level & 2) {
			cv::imshow("Debug", image);
			static int waitTime = Config::getConfig().get<int>("config.debugging.wait_key_time", 0);
			if (waitTime) cv::waitKey(waitTime);
			else cv::waitKey();
		}

		auto startTime = boost::posix_time::microsec_clock::local_time();

		std::vector<Recognizer::RecognitionResult> results = recognizer->recognize(image, deckInfo.state | gameInfo.state | drawInfo.state);

		if (param_debug_level & 1) {
			auto endTime = boost::posix_time::microsec_clock::local_time();
			boost::posix_time::time_duration diff = endTime - startTime;
			const auto elapsed = diff.total_milliseconds();
			if (stream->isLivestream()) {
				HS_INFO << "Processed frame in " << elapsed << "ms" << std::endl;
			} else {
				HS_INFO << "Processed frame " << stream->getFramePos() << " of stream " << stream->getStreamIndex() << " in " << elapsed << "ms" << std::endl;
			}
		}

		passedFrames++;
		if (results.empty()) continue;

		stateMutex.lock();
		for (auto& result : results) {
			if (RECOGNIZER_DRAFT_CLASS_PICK == result.sourceRecognizer && (deckInfo.state & RECOGNIZER_DRAFT_CLASS_PICK)) {
				deck.clear();
				deckInfo.clear();
				disable(deckInfo.state, RECOGNIZER_DRAFT_CLASS_PICK);
				enable(deckInfo.state, RECOGNIZER_DRAFT_CARD_PICK);
				HS_INFO << "new draft: " << db->heroes[result.results[0]].name << ", " << db->heroes[result.results[1]].name << ", " << db->heroes[result.results[2]].name << std::endl;
				bot->message("!score -arena");
				winsLosses = std::make_pair<int, int>(0,0);

				if (internalState & INTERNAL_STRAWPOLLING) {
					bot->message("!subon");
					bool success = false;
					std::vector<std::string> classNames;
					for (auto& r : result.results) {
						classNames.push_back(db->heroes[r].name);
					}
					for (int i = 0; i <= MSG_CLASS_POLL_ERROR_RETRY_COUNT && !success; i++) {
						std::string strawpoll = SystemInterface::createStrawpoll((boost::format(MSG_CLASS_POLL) % sName).str(), classNames);
						if (strawpoll.empty()) {
							bot->message((boost::format(MSG_CLASS_POLL_ERROR) % (MSG_CLASS_POLL_ERROR_RETRY_COUNT - i)).str());
						} else {
							bot->message((boost::format(MSG_CLASS_POLL_VOTE) % sName % strawpoll).str());
							bot->repeat_message((boost::format(MSG_CLASS_POLL_VOTE_REPEAT) % strawpoll).str(),
									5, 25, 7);
							bot->message("!suboff", 120);
							success = true;
						}
					}

					if (!success) {
						bot->message(MSG_CLASS_POLL_ERROR_GIVEUP);
					}
				}
			}
			else if (RECOGNIZER_DRAFT_CARD_PICK == result.sourceRecognizer && (deckInfo.state & RECOGNIZER_DRAFT_CARD_PICK)) {
				const size_t last = deck.setHistory.size() - 1;
				bool isNew = deck.pickHistory.size() == 0 && last == -1;
				for (size_t i = 0; i < result.results.size() && !isNew; i++) {
					//is there at least one new card in the current recognized pick?
					isNew |= (result.results[i] != deck.setHistory[last][i].id);
				}

				if (isNew) {
					deck.addSet(db->cards[result.results[0]], db->cards[result.results[1]], db->cards[result.results[2]]);
					if ((deckInfo.state & RECOGNIZER_DRAFT_CARD_CHOSEN) && deck.setHistory.size() == deck.pickHistory.size() + 2) {
						deck.addUnknownPick();
						HS_WARNING << "Missed pick " << deck.cards.size() << std::endl;
					}
					enable(deckInfo.state, RECOGNIZER_DRAFT_CLASS_PICK);
					enable(deckInfo.state, RECOGNIZER_DRAFT_CARD_CHOSEN);
					HS_INFO << "pick " << deck.getCardCount() + 1 << ": " + db->cards[result.results[0]].name + ", " + db->cards[result.results[1]].name + ", " << db->cards[result.results[2]].name << std::endl;
				}
			}
			else if (RECOGNIZER_DRAFT_CARD_CHOSEN  == result.sourceRecognizer && (deckInfo.state & RECOGNIZER_DRAFT_CARD_CHOSEN)) {
				enable(deckInfo.state, RECOGNIZER_DRAFT_CLASS_PICK);
				enable(deckInfo.state, RECOGNIZER_DRAFT_CARD_PICK);
				disable(deckInfo.state, RECOGNIZER_DRAFT_CARD_CHOSEN);

				Card c = deck.setHistory.back()[result.results[0]];
				HS_INFO << "picked " << c.name << std::endl;
				deck.addPickedCard(c);
				shouldUpdateDeck = true;

				if (deck.isComplete()) {
					disable(deckInfo.state, RECOGNIZER_DRAFT_CARD_PICK);
					deckInfo.msg = createDeckURLs();
					bot->message(deckInfo.msg);
				}
			}
			else if (RECOGNIZER_GAME_CLASS_SHOW  == result.sourceRecognizer && (gameInfo.state & RECOGNIZER_GAME_CLASS_SHOW)) {
				enable(gameInfo.state, RECOGNIZER_GAME_COIN);
				disable(gameInfo.state, RECOGNIZER_GAME_CLASS_SHOW);
				gameInfo.player = db->heroes[result.results[0]].name;
				gameInfo.opponent = db->heroes[result.results[1]].name;
				HS_INFO << "New Game: " << gameInfo.player << " vs. " << gameInfo.opponent << std::endl;
				if (shouldUpdateDeck && (internalState & INTERNAL_APICALLING)) {
					SystemInterface::callAPI(api.submitDeckFormat, list_of(deck.heroClass)(deck.createInternalRepresentation()));
					shouldUpdateDeck = false;
				}
			}
			else if (RECOGNIZER_GAME_COIN == result.sourceRecognizer && (gameInfo.state & RECOGNIZER_GAME_COIN)) {
				enable(gameInfo.state, RECOGNIZER_GAME_END);
				enable(gameInfo.state, RECOGNIZER_GAME_CLASS_SHOW);
				disable(gameInfo.state, RECOGNIZER_GAME_COIN);
				gameInfo.fs = (result.results[0] == RESULT_GAME_COIN_FIRST)? "1" : "2";
				if (internalState & INTERNAL_SCORING) {
					bot->message((boost::format(MSG_GAME_START) % gameInfo.player % gameInfo.opponent % gameInfo.fs).str());
				}
				if (param_debug_level & 4) {
					const std::string& time = boost::lexical_cast<std::string>(boost::posix_time::microsec_clock::local_time().time_of_day().total_milliseconds());
					std::string name = "coin" + gameInfo.fs + time + ".png";
					SystemInterface::saveImage(image, name);
				}
				if ((internalState & INTERNAL_DRAWHANDLING) && gameInfo.fs == "1") {
					enable(drawInfo.state, RECOGNIZER_GAME_DRAW_INIT_1);
				} else if ((internalState & INTERNAL_DRAWHANDLING) && gameInfo.fs == "2") {
					enable(drawInfo.state, RECOGNIZER_GAME_DRAW_INIT_2);
				}
				drawInfo.latestDraw = -1;
				if (internalState & INTERNAL_APICALLING) SystemInterface::callAPI(api.resetDrawsFormat, std::vector<std::string>());
				deck.resetDraws();
			}
			else if (RECOGNIZER_GAME_END == result.sourceRecognizer && (gameInfo.state & RECOGNIZER_GAME_END)) {
				enable(gameInfo.state, RECOGNIZER_GAME_CLASS_SHOW);
				disable(gameInfo.state, RECOGNIZER_GAME_END);
				gameInfo.end = (result.results[0] == RESULT_GAME_END_VICTORY)? "w" : "l";
				if (gameInfo.end == "w") winsLosses.first++;
				else winsLosses.second++;

				if (internalState & INTERNAL_SCORING) {
					bot->message((boost::format(MSG_GAME_END) % gameInfo.end).str());
				}

				if (winsLosses.first == 12 || winsLosses.second == 3) {
					bot->message("!score -constructed", 0);
				}
				if (param_debug_level & 4) {
					const std::string& time = boost::lexical_cast<std::string>(boost::posix_time::microsec_clock::local_time().time_of_day().total_milliseconds());
					std::string name = gameInfo.end + time + ".png";
					SystemInterface::saveImage(image, name);
				}
			}
			else if (RECOGNIZER_GAME_DRAW_INIT_1 == result.sourceRecognizer && (drawInfo.state & RECOGNIZER_GAME_DRAW_INIT_1)) {
				drawInfo.initialDraw = result.results;
			}
			else if (RECOGNIZER_GAME_DRAW_INIT_2 == result.sourceRecognizer && (drawInfo.state & RECOGNIZER_GAME_DRAW_INIT_2)) {
				drawInfo.initialDraw = result.results;
			}
			else if (RECOGNIZER_GAME_DRAW == result.sourceRecognizer && (drawInfo.state & RECOGNIZER_GAME_DRAW) && passedFrames.load() >= PASSED_FRAMES_THRESHOLD) {
				bool pass = result.results[0] == currentCard.first && ++currentCard.second >= PASSED_CARD_RECOGNITIONS;
				if (result.results[0] != currentCard.first) currentCard.second = 0;
				currentCard.first = result.results[0];
				if (pass) {
					passedFrames = 0;
					currentCard.second = 0;
					currentCard.first = -1;
					bool newCards = false;
					if (drawInfo.latestDraw == -1) {
						std::vector<std::string> initDrawNames;
						for (const auto& id : drawInfo.initialDraw) {
							newCards |= deck.draw(db->cards[id], internalState & INTERNAL_BUILDFROMDRAWS);
							initDrawNames.push_back(db->cards[id].name);
							if (internalState & INTERNAL_APICALLING) SystemInterface::callAPI(api.drawCardFormat, list_of((boost::format("%03d") % id).str()));
						}
						std::string initDraw = boost::algorithm::join(initDrawNames, "; ");
//						bot->message((boost::format(MSG_INITIAL_DRAW) % initDraw).str());
						drawInfo.initialDraw.clear();
						disable(drawInfo.state, RECOGNIZER_GAME_DRAW_INIT_1);
						disable(drawInfo.state, RECOGNIZER_GAME_DRAW_INIT_2);
					}
//					bot->message((boost::format(MSG_DRAW) % db->cards[result.results[0]].name).str());
					newCards |= deck.draw(db->cards[result.results[0]], internalState & INTERNAL_BUILDFROMDRAWS);
					shouldUpdateDeck |= newCards;
					if (internalState & INTERNAL_APICALLING) SystemInterface::callAPI(api.drawCardFormat, list_of((boost::format("%03d") % result.results[0]).str()));
					drawInfo.latestDraw = result.results[0];

					if (newCards && deck.isComplete()) {
						deckInfo.msg = createDeckURLs();
						bot->message(deckInfo.msg);
					}
				}
			}
		}
		stateMutex.unlock();
	}

	HS_ERROR << "an error while reading a frame occured" << std::endl;
}

std::string StreamManager::processCommand(const std::string& user, const std::string& cmd, bool isMod, bool isSuperUser) {
	if (cmd.empty() || cmd.find("!") != 0) return std::string("");
	return cp->process(user, cmd, isMod, isSuperUser);
}

std::string StreamManager::createDeckURLs() {
	std::string deckImage = SystemInterface::createImgur(deck.createImageRepresentation());
	std::string deckString = SystemInterface::createHastebin(deck.createTextRepresentation());
	return (boost::format(CMD_DECK_FORMAT) % sName % deckImage % deckString).str();
}

}
