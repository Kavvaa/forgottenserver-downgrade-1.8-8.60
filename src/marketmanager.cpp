// Copyright 2026 The Forgotten Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#include "otpch.h"

#include "marketmanager.h"

#include "configmanager.h"
#include "const.h"
#include "container.h"
#include "database.h"
#include "depotchest.h"
#include "depotlocker.h"
#include "game.h"
#include "inbox.h"
#include "iologindata.h"
#include "item.h"
#include "logger.h"
#include "player.h"

#include <fmt/format.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

extern Game g_game;

namespace {

constexpr uint8_t MARKET_ACTION_BUY = 0;
constexpr uint8_t MARKET_ACTION_SELL = 1;

constexpr uint8_t MARKET_STATE_CANCELLED = 1;
constexpr uint8_t MARKET_STATE_EXPIRED = 2;
constexpr uint8_t MARKET_STATE_ACCEPTED = 3;

constexpr uint16_t MARKET_REQUEST_MY_OFFERS = 0xFFFE;
constexpr uint32_t MARKET_MAX_OFFERS = 100;
constexpr uint32_t MARKET_MAX_AMOUNT = 2000;
constexpr uint32_t MARKET_MAX_AMOUNT_STACKABLE = 64000;
constexpr uint32_t MARKET_MAX_PRICE = 999999999;
constexpr uint16_t MARKET_DEPOT_BOX_FIRST = 1;
constexpr uint16_t MARKET_DEPOT_BOX_LAST = 15;

struct MarketDbOffer
{
	uint32_t id = 0;
	uint32_t playerId = 0;
	uint8_t sale = 0;
	uint16_t itemId = 0;
	uint16_t amount = 0;
	uint64_t created = 0;
	bool anonymous = false;
	uint32_t price = 0;
	std::string playerName;
};

MarketOperationResult makeResult(bool success, std::string message, uint16_t browseId = 0,
                                 std::string counterpartyName = {})
{
	MarketOperationResult result;
	result.success = success;
	result.message = std::move(message);
	result.browseId = browseId;
	result.counterpartyName = std::move(counterpartyName);
	return result;
}

uint64_t getOfferDuration()
{
	return static_cast<uint64_t>(std::max<int32_t>(1, ConfigManager::getInteger(ConfigManager::MARKET_OFFER_DURATION)));
}

uint64_t getNow()
{
	return static_cast<uint64_t>(time(nullptr));
}

uint32_t calculateFee(uint32_t price, uint16_t amount)
{
	uint64_t fee = (static_cast<uint64_t>(price) * amount + 99) / 100;
	if (fee < 20) {
		return 20;
	}
	if (fee > 1000) {
		return 1000;
	}
	return static_cast<uint32_t>(fee);
}

bool isBlockedMarketItem(uint16_t itemId)
{
	return itemId == ITEM_GOLD_COIN || itemId == ITEM_PLATINUM_COIN || itemId == ITEM_CRYSTAL_COIN ||
	       itemId == ITEM_GOLD_NUGGET || itemId == ITEM_MARKET || itemId == ITEM_INBOX || itemId == ITEM_LOCKER ||
	       itemId == ITEM_DEPOT;
}

bool isMarketableItemType(uint16_t itemId)
{
	if (itemId == 0 || isBlockedMarketItem(itemId)) {
		return false;
	}

	const ItemType& itemType = Item::items[itemId];
	if (itemType.id == 0 || itemType.group == ITEM_GROUP_DEPRECATED) {
		return false;
	}

	if (!itemType.isPickupable() || itemType.isFluidContainer() || itemType.isSplash() || itemType.charges != 0) {
		return false;
	}

	return true;
}

bool isDepotMarketItem(const Item& item, uint16_t itemId)
{
	return item.getID() == itemId && item.getContainer() == nullptr;
}

uint16_t getItemTradeCount(const Item& item)
{
	return item.isStackable() ? item.getItemCount() : 1;
}

uint16_t resolveMarketDepotId(Player* player, uint16_t depotId)
{
	if (depotId != 0 || !player) {
		return depotId;
	}

	if (player->getLastDepotId() > 0) {
		return static_cast<uint16_t>(player->getLastDepotId());
	}

	if (Town* town = player->getTown()) {
		return static_cast<uint16_t>(town->getID());
	}

	return 0;
}

Container* findDepotBox(Container* root, uint16_t boxIndex)
{
	if (!root || boxIndex < MARKET_DEPOT_BOX_FIRST || boxIndex > MARKET_DEPOT_BOX_LAST) {
		return nullptr;
	}

	const uint16_t boxId = static_cast<uint16_t>(ITEM_DEPOT_BOX_1 + boxIndex - 1);
	for (const auto& item : root->getItemList()) {
		if (item->getID() == boxId) {
			return item->getContainer();
		}

		Container* child = item->getContainer();
		if (child && (item->getID() == ITEM_DEPOT || item->getID() == ITEM_LOCKER)) {
			if (Container* box = findDepotBox(child, boxIndex)) {
				return box;
			}
		}
	}
	return nullptr;
}

Container* getDepotBox(Player* player, uint16_t depotId, uint16_t boxIndex)
{
	if (!player || boxIndex < MARKET_DEPOT_BOX_FIRST || boxIndex > MARKET_DEPOT_BOX_LAST) {
		return nullptr;
	}

	const uint16_t resolvedDepotId = resolveMarketDepotId(player, depotId);
	if (resolvedDepotId == 0) {
		return nullptr;
	}

	if (DepotLocker* locker = player->getDepotLocker(resolvedDepotId)) {
		if (Container* box = findDepotBox(locker, boxIndex)) {
			return box;
		}
	}

	DepotChest* chest = player->getDepotChest(resolvedDepotId, true);
	return findDepotBox(chest, boxIndex);
}

uint32_t getPlainDepotItemAmount(Player* player, uint16_t depotId, uint16_t itemId)
{
	uint32_t amount = 0;
	for (uint16_t boxIndex = MARKET_DEPOT_BOX_FIRST; boxIndex <= MARKET_DEPOT_BOX_LAST; ++boxIndex) {
		Container* box = getDepotBox(player, depotId, boxIndex);
		if (!box) {
			continue;
		}

		for (const std::shared_ptr<Item>& itemPtr : box->getItems(true)) {
			Item* item = itemPtr.get();
			if (item && isDepotMarketItem(*item, itemId)) {
				amount += getItemTradeCount(*item);
				if (amount >= std::numeric_limits<uint16_t>::max()) {
					return std::numeric_limits<uint16_t>::max();
				}
			}
		}
	}
	return amount;
}

bool removePlainDepotItems(Player* player, uint16_t depotId, uint16_t itemId, uint32_t amount)
{
	if (!player || amount == 0) {
		return false;
	}

	uint32_t remaining = amount;
	for (uint16_t boxIndex = MARKET_DEPOT_BOX_FIRST; boxIndex <= MARKET_DEPOT_BOX_LAST && remaining > 0; ++boxIndex) {
		Container* box = getDepotBox(player, depotId, boxIndex);
		if (!box) {
			continue;
		}

		for (const std::shared_ptr<Item>& itemPtr : box->getItems(true)) {
			Item* item = itemPtr.get();
			if (!item || !isDepotMarketItem(*item, itemId)) {
				continue;
			}

			const uint32_t count = std::min<uint32_t>(remaining, getItemTradeCount(*item));
			if (g_game.internalRemoveItem(item, static_cast<int32_t>(count)) != RETURNVALUE_NOERROR) {
				return false;
			}

			remaining -= count;
			if (remaining == 0) {
				return true;
			}
		}
	}
	return false;
}

bool addPlainItemsToContainer(Container* container, uint16_t itemId, uint32_t amount)
{
	if (!container || amount == 0) {
		return false;
	}

	const ItemType& itemType = Item::items[itemId];
	const uint32_t stackSize = itemType.stackable ? std::max<uint32_t>(1, itemType.stackSize) : 1;
	uint32_t remaining = amount;
	while (remaining > 0) {
		const uint16_t count = static_cast<uint16_t>(itemType.stackable ? std::min<uint32_t>(remaining, stackSize) : 1);
		std::shared_ptr<Item> item = Item::CreateItem(itemId, count);
		if (!item) {
			return false;
		}

		if (g_game.internalAddItem(container, item.get(), INDEX_WHEREEVER, FLAG_NOLIMIT) != RETURNVALUE_NOERROR) {
			return false;
		}
		remaining -= count;
	}
	return true;
}

bool addPlainItemsToDepot(Player* player, uint16_t depotId, uint16_t itemId, uint32_t amount)
{
	for (uint16_t boxIndex = MARKET_DEPOT_BOX_FIRST; boxIndex <= MARKET_DEPOT_BOX_LAST; ++boxIndex) {
		Container* box = getDepotBox(player, depotId, boxIndex);
		if (box) {
			return addPlainItemsToContainer(box, itemId, amount);
		}
	}
	return false;
}

bool removePlainInboxItems(Player* player, uint16_t itemId, uint32_t amount)
{
	Inbox* inbox = player ? player->getInbox() : nullptr;
	if (!inbox || amount == 0) {
		return false;
	}

	uint32_t remaining = amount;
	for (const std::shared_ptr<Item>& itemPtr : inbox->getItems(true)) {
		Item* item = itemPtr.get();
		if (!item || item->getID() != itemId) {
			continue;
		}

		const uint32_t count = std::min<uint32_t>(remaining, getItemTradeCount(*item));
		if (g_game.internalRemoveItem(item, static_cast<int32_t>(count)) != RETURNVALUE_NOERROR) {
			return false;
		}

		remaining -= count;
		if (remaining == 0) {
			return true;
		}
	}
	return false;
}

bool addPlainItemsToInbox(Player* player, uint16_t itemId, uint32_t amount)
{
	Inbox* inbox = player ? player->getInbox() : nullptr;
	return inbox && addPlainItemsToContainer(inbox, itemId, amount);
}

std::optional<uint64_t> getPlayerBalanceForUpdate(Database& db, uint32_t playerId)
{
	DBResult_ptr result = db.storeQuery(fmt::format("SELECT `balance` FROM `players` WHERE `id` = {:d} FOR UPDATE", playerId));
	if (!result) {
		return std::nullopt;
	}
	return result->getNumber<uint64_t>("balance");
}

struct MarketDebit
{
	uint64_t inventory = 0;
	uint64_t bank = 0;
	uint64_t newBalance = 0;
};

bool debitPlayerMarketMoney(Database& db, Player* player, uint64_t amount, MarketDebit& debit)
{
	if (!player) {
		return false;
	}

	auto balance = getPlayerBalanceForUpdate(db, player->getGUID());
	if (!balance) {
		return false;
	}

	const uint64_t inventoryMoney = player->getMoney();
	if (inventoryMoney < amount && *balance < amount - inventoryMoney) {
		return false;
	}

	debit.inventory = std::min<uint64_t>(inventoryMoney, amount);
	debit.bank = amount - debit.inventory;
	debit.newBalance = *balance - debit.bank;

	if (debit.bank > 0) {
		if (!db.executeQuery(fmt::format(
		        "UPDATE `players` SET `balance` = `balance` - {:d} WHERE `id` = {:d} AND `balance` >= {:d}",
		        debit.bank, player->getGUID(), debit.bank))) {
			return false;
		}

		if (db.getAffectedRows() != 1) {
			LOG_ERROR(fmt::format("[MarketManager] Debit affected {} rows for player {}", db.getAffectedRows(),
			                      player->getGUID()));
			return false;
		}
	}

	if (debit.inventory > 0 && !g_game.removeMoney(player, debit.inventory)) {
		return false;
	}
	return true;
}

void rollbackMarketDebit(Player* player, const MarketDebit& debit)
{
	if (player && debit.inventory > 0) {
		g_game.addMoney(player, debit.inventory, FLAG_NOLIMIT);
	}
}

bool creditPlayerBalance(Database& db, uint32_t playerId, uint64_t amount, uint64_t& newBalance)
{
	auto balance = getPlayerBalanceForUpdate(db, playerId);
	if (!balance || amount > std::numeric_limits<uint64_t>::max() - *balance) {
		return false;
	}

	if (!db.executeQuery(fmt::format("UPDATE `players` SET `balance` = `balance` + {:d} WHERE `id` = {:d}",
	                                amount, playerId))) {
		return false;
	}

	if (db.getAffectedRows() != 1) {
		LOG_ERROR(fmt::format("[MarketManager] Credit affected {} rows for player {}", db.getAffectedRows(), playerId));
		return false;
	}

	newBalance = *balance + amount;
	return true;
}

uint32_t getActiveOfferCount(Database& db, uint32_t playerId)
{
	DBResult_ptr result =
	    db.storeQuery(fmt::format("SELECT COUNT(*) AS `total` FROM `market_offers` WHERE `player_id` = {:d}", playerId));
	return result ? result->getNumber<uint32_t>("total") : MARKET_MAX_OFFERS;
}

std::optional<MarketDbOffer> getOfferForUpdate(Database& db, uint32_t offerId)
{
	DBResult_ptr result = db.storeQuery(fmt::format(
	    "SELECT mo.`id`, mo.`player_id`, mo.`sale`, mo.`itemtype`, mo.`amount`, mo.`created`, mo.`anonymous`, mo.`price`, "
	    "p.`name` AS `player_name` FROM `market_offers` mo INNER JOIN `players` p ON p.`id` = mo.`player_id` "
	    "WHERE mo.`id` = {:d} LIMIT 1 FOR UPDATE",
	    offerId));
	if (!result) {
		return std::nullopt;
	}

	MarketDbOffer offer;
	offer.id = result->getNumber<uint32_t>("id");
	offer.playerId = result->getNumber<uint32_t>("player_id");
	offer.sale = result->getNumber<uint8_t>("sale");
	offer.itemId = result->getNumber<uint16_t>("itemtype");
	offer.amount = result->getNumber<uint16_t>("amount");
	offer.created = result->getNumber<uint64_t>("created");
	offer.anonymous = result->getNumber<uint16_t>("anonymous") != 0;
	offer.price = result->getNumber<uint32_t>("price");
	offer.playerName = std::string(result->getString("player_name"));
	return offer;
}

bool claimOffer(Database& db, const MarketDbOffer& offer, uint16_t amount)
{
	if (amount >= offer.amount) {
		if (!db.executeQuery(fmt::format("DELETE FROM `market_offers` WHERE `id` = {:d} AND `amount` = {:d}", offer.id,
		                                offer.amount))) {
			return false;
		}
	} else {
		if (!db.executeQuery(fmt::format("UPDATE `market_offers` SET `amount` = `amount` - {:d} WHERE `id` = {:d} "
		                                "AND `amount` >= {:d}",
		                                amount, offer.id, amount))) {
			return false;
		}
	}

	if (db.getAffectedRows() != 1) {
		LOG_ERROR(fmt::format("[MarketManager] Offer claim affected {} rows for offer {}", db.getAffectedRows(), offer.id));
		return false;
	}
	return true;
}

bool deleteOffer(Database& db, uint32_t offerId, uint32_t playerId)
{
	if (!db.executeQuery(
	        fmt::format("DELETE FROM `market_offers` WHERE `id` = {:d} AND `player_id` = {:d}", offerId, playerId))) {
		return false;
	}

	if (db.getAffectedRows() != 1) {
		LOG_ERROR(fmt::format("[MarketManager] Delete affected {} rows for offer {}", db.getAffectedRows(), offerId));
		return false;
	}
	return true;
}

bool insertHistory(Database& db, const MarketDbOffer& offer, uint16_t amount, uint8_t state)
{
	const uint64_t now = getNow();
	const uint64_t expiresAt = offer.created + getOfferDuration();
	if (!db.executeQuery(fmt::format(
	        "INSERT INTO `market_history` (`player_id`, `sale`, `itemtype`, `amount`, `price`, `expires_at`, `inserted`, "
	        "`state`) VALUES ({:d}, {:d}, {:d}, {:d}, {:d}, {:d}, {:d}, {:d})",
	        offer.playerId, offer.sale, offer.itemId, amount, offer.price, expiresAt, now, state))) {
		return false;
	}

	if (db.getAffectedRows() != 1) {
		LOG_ERROR(fmt::format("[MarketManager] History insert affected {} rows for offer {}", db.getAffectedRows(),
		                      offer.id));
		return false;
	}
	return true;
}

bool updateStatistics(Database& db, const MarketDbOffer& offer, uint16_t amount)
{
	const uint64_t totalPrice = static_cast<uint64_t>(offer.price) * amount;
	const uint64_t day = (getNow() / 86400) * 86400;
	return db.executeQuery(fmt::format(
	    "INSERT INTO `market_statistics` (`itemtype`, `sale`, `day`, `transactions`, `total_price`, `highest_price`, "
	    "`lowest_price`) VALUES ({:d}, {:d}, {:d}, 1, {:d}, {:d}, {:d}) ON DUPLICATE KEY UPDATE "
	    "`transactions` = `transactions` + 1, `total_price` = `total_price` + VALUES(`total_price`), "
	    "`highest_price` = GREATEST(`highest_price`, VALUES(`highest_price`)), "
	    "`lowest_price` = IF(`lowest_price` = 0, VALUES(`lowest_price`), LEAST(`lowest_price`, VALUES(`lowest_price`)))",
	    offer.itemId, offer.sale, day, totalPrice, offer.price, offer.price));
}

int32_t getNextInboxSid(Database& db, uint32_t playerId)
{
	DBResult_ptr result = db.storeQuery(
	    fmt::format("SELECT COALESCE(MAX(`sid`), 100) AS `sid` FROM `player_inboxitems` WHERE `player_id` = {:d}",
	                playerId));
	return result ? result->getNumber<int32_t>("sid") + 1 : 101;
}

std::optional<int32_t> getOfflineInboxDepotId(Database& db, uint32_t playerId)
{
	DBResult_ptr result = db.storeQuery(
	    fmt::format("SELECT `town_id` FROM `players` WHERE `id` = {:d} LIMIT 1 FOR UPDATE", playerId));
	if (!result) {
		return std::nullopt;
	}

	const int32_t townId = result->getNumber<int32_t>("town_id");
	if (townId > 0) {
		return townId;
	}

	return std::max<int32_t>(1, ConfigManager::getInteger(ConfigManager::NEW_PLAYER_TOWN_ID));
}

bool insertOfflineInboxItems(Database& db, uint32_t playerId, uint16_t itemId, uint32_t amount)
{
	const ItemType& itemType = Item::items[itemId];
	const uint32_t stackSize = itemType.stackable ? std::max<uint32_t>(1, itemType.stackSize) : 1;
	const std::optional<int32_t> depotId = getOfflineInboxDepotId(db, playerId);
	if (!depotId) {
		return false;
	}

	int32_t sid = getNextInboxSid(db, playerId);
	uint32_t remaining = amount;

	while (remaining > 0) {
		const uint16_t count = static_cast<uint16_t>(itemType.stackable ? std::min<uint32_t>(remaining, stackSize) : 1);
		if (!db.executeQuery(fmt::format(
		        "INSERT INTO `player_inboxitems` (`player_id`, `sid`, `pid`, `itemtype`, `count`, `attributes`) "
		        "VALUES ({:d}, {:d}, {:d}, {:d}, {:d}, {:s})",
		        playerId, sid, *depotId, itemId, count, db.escapeBlob("", 0)))) {
			return false;
		}
		if (db.getAffectedRows() != 1) {
			LOG_ERROR(fmt::format("[MarketManager] Inbox insert affected {} rows for player {}", db.getAffectedRows(),
			                      playerId));
			return false;
		}

		remaining -= count;
		++sid;
	}
	return true;
}

bool deliverItemsToPlayer(Database& db, uint32_t playerId, uint16_t itemId, uint32_t amount, uint32_t& onlinePlayerId)
{
	if (std::shared_ptr<Player> target = g_game.getPlayerByGUID(playerId)) {
		if (!addPlainItemsToInbox(target.get(), itemId, amount)) {
			return false;
		}
		if (!IOLoginData::savePlayerInboxItems(target.get())) {
			removePlainInboxItems(target.get(), itemId, amount);
			return false;
		}
		onlinePlayerId = target->getID();
		return true;
	}

	return insertOfflineInboxItems(db, playerId, itemId, amount);
}

void setOnlineBalance(uint32_t playerId, uint64_t balance)
{
	if (std::shared_ptr<Player> player = g_game.getPlayerByGUID(playerId)) {
		player->setBankBalance(balance);
	}
}

bool validateOfferPayload(uint8_t action, uint16_t itemId, uint16_t amount, uint32_t price, std::string& message)
{
	if (action != MARKET_ACTION_BUY && action != MARKET_ACTION_SELL) {
		message = "Invalid offer type.";
		return false;
	}

	if (!isMarketableItemType(itemId)) {
		message = "This item cannot be traded on the market.";
		return false;
	}

	const ItemType& itemType = Item::items[itemId];
	const uint32_t maxAmount = itemType.stackable ? MARKET_MAX_AMOUNT_STACKABLE : MARKET_MAX_AMOUNT;
	if (amount == 0 || amount > maxAmount) {
		message = "Invalid amount.";
		return false;
	}

	const uint64_t totalPrice = static_cast<uint64_t>(price) * amount;
	if (price == 0 || price > MARKET_MAX_PRICE || totalPrice > MARKET_MAX_PRICE) {
		message = "Invalid price.";
		return false;
	}

	return true;
}

} // namespace

MarketOperationResult MarketManager::createOffer(Player* player, uint8_t action, uint16_t itemId, uint16_t amount,
                                                 uint32_t price, bool anonymous, uint16_t depotId)
{
	if (!player) {
		return makeResult(false, "Player not found.");
	}

	std::string validationMessage;
	if (!validateOfferPayload(action, itemId, amount, price, validationMessage)) {
		return makeResult(false, validationMessage);
	}

	Database& db = Database::getInstance();
	DBTransaction transaction;
	if (!transaction.begin()) {
		return makeResult(false, "Could not start market transaction.");
	}

	const uint32_t playerId = player->getGUID();
	if (getActiveOfferCount(db, playerId) >= MARKET_MAX_OFFERS) {
		return makeResult(false, "You have too many active market offers.");
	}

	const uint64_t totalPrice = static_cast<uint64_t>(price) * amount;
	const uint64_t debitAmount = action == MARKET_ACTION_BUY ? totalPrice + calculateFee(price, amount) : calculateFee(price, amount);
	MarketDebit marketDebit;
	if (!debitPlayerMarketMoney(db, player, debitAmount, marketDebit)) {
		return makeResult(false, action == MARKET_ACTION_BUY ? "You do not have enough money for this buy offer."
		                                                     : "You do not have enough money to pay the market fee.");
	}

	bool removedDepotItems = false;
	if (action == MARKET_ACTION_SELL) {
		if (getPlainDepotItemAmount(player, depotId, itemId) < amount) {
			rollbackMarketDebit(player, marketDebit);
			return makeResult(false, "You do not have enough plain items in this depot.");
		}

		if (!removePlainDepotItems(player, depotId, itemId, amount)) {
			rollbackMarketDebit(player, marketDebit);
			return makeResult(false, "Could not reserve the depot items.");
		}
		removedDepotItems = true;

		if (!IOLoginData::savePlayerDepotItems(player)) {
			addPlainItemsToDepot(player, depotId, itemId, amount);
			rollbackMarketDebit(player, marketDebit);
			return makeResult(false, "Could not persist depot item reservation.");
		}
	}

	const uint64_t now = getNow();
	if (!db.executeQuery(fmt::format(
	        "INSERT INTO `market_offers` (`player_id`, `sale`, `itemtype`, `amount`, `created`, `anonymous`, `price`) "
	        "VALUES ({:d}, {:d}, {:d}, {:d}, {:d}, {:d}, {:d})",
	        playerId, action, itemId, amount, now, anonymous ? 1 : 0, price))) {
		if (removedDepotItems) {
			addPlainItemsToDepot(player, depotId, itemId, amount);
		}
		rollbackMarketDebit(player, marketDebit);
		return makeResult(false, "Could not create the market offer.");
	}

	if (db.getAffectedRows() != 1) {
		if (removedDepotItems) {
			addPlainItemsToDepot(player, depotId, itemId, amount);
		}
		rollbackMarketDebit(player, marketDebit);
		LOG_ERROR(fmt::format("[MarketManager] Offer insert affected {} rows for player {}", db.getAffectedRows(), playerId));
		return makeResult(false, "Could not create the market offer.");
	}

	if (!transaction.commit()) {
		if (removedDepotItems) {
			addPlainItemsToDepot(player, depotId, itemId, amount);
		}
		rollbackMarketDebit(player, marketDebit);
		return makeResult(false, "Could not commit the market offer.");
	}

	player->setBankBalance(marketDebit.newBalance);
	return makeResult(true, "Market offer created.", itemId);
}

MarketOperationResult MarketManager::cancelOffer(Player* player, uint32_t offerId, uint16_t /*depotId*/)
{
	if (!player || offerId == 0) {
		return makeResult(false, "Market offer not found.");
	}

	Database& db = Database::getInstance();
	DBTransaction transaction;
	if (!transaction.begin()) {
		return makeResult(false, "Could not start market transaction.");
	}

	std::optional<MarketDbOffer> offer = getOfferForUpdate(db, offerId);
	if (!offer || offer->playerId != player->getGUID()) {
		return makeResult(false, "Market offer not found.");
	}

	uint64_t newBalance = player->getBankBalance();
	bool addedItems = false;
	if (offer->sale == MARKET_ACTION_BUY) {
		if (!creditPlayerBalance(db, offer->playerId, static_cast<uint64_t>(offer->price) * offer->amount, newBalance)) {
			return makeResult(false, "Could not refund the market offer.");
		}
	} else {
		if (!addPlainItemsToInbox(player, offer->itemId, offer->amount)) {
			return makeResult(false, "Could not return the market items.");
		}
		addedItems = true;

		if (!IOLoginData::savePlayerInboxItems(player)) {
			removePlainInboxItems(player, offer->itemId, offer->amount);
			return makeResult(false, "Could not persist returned market items.");
		}
	}

	if (!deleteOffer(db, offer->id, offer->playerId) || !insertHistory(db, *offer, offer->amount, MARKET_STATE_CANCELLED)) {
		if (addedItems) {
			removePlainInboxItems(player, offer->itemId, offer->amount);
		}
		return makeResult(false, "Could not cancel the market offer.");
	}

	if (!transaction.commit()) {
		if (addedItems) {
			removePlainInboxItems(player, offer->itemId, offer->amount);
		}
		return makeResult(false, "Could not commit market cancellation.");
	}

	if (offer->sale == MARKET_ACTION_BUY) {
		player->setBankBalance(newBalance);
	}
	return makeResult(true, "Market offer cancelled.", MARKET_REQUEST_MY_OFFERS);
}

MarketOperationResult MarketManager::acceptOffer(Player* player, uint32_t offerId, uint16_t amount, uint16_t depotId)
{
	if (!player || offerId == 0 || amount == 0) {
		return makeResult(false, "Market offer not found.");
	}

	Database& db = Database::getInstance();
	DBTransaction transaction;
	if (!transaction.begin()) {
		return makeResult(false, "Could not start market transaction.");
	}

	std::optional<MarketDbOffer> offer = getOfferForUpdate(db, offerId);
	if (!offer) {
		return makeResult(false, "Market offer not found.");
	}
	if (offer->playerId == player->getGUID()) {
		return makeResult(false, "You cannot accept your own market offer.");
	}
	if (amount > offer->amount) {
		return makeResult(false, "Invalid amount.");
	}
	if (offer->created + getOfferDuration() <= getNow()) {
		return makeResult(false, "Market offer has expired.");
	}

	const uint64_t totalPrice = static_cast<uint64_t>(offer->price) * amount;
	uint64_t acceptorNewBalance = player->getBankBalance();
	uint64_t ownerNewBalance = 0;
	uint32_t deliveredOnlinePlayerId = 0;
	bool removedDepotItems = false;
	bool deliveredItems = false;
	bool debitedMarketMoney = false;
	MarketDebit acceptorDebit;

	if (!claimOffer(db, *offer, amount)) {
		return makeResult(false, "Market offer is no longer available.");
	}

	if (offer->sale == MARKET_ACTION_SELL) {
		if (!debitPlayerMarketMoney(db, player, totalPrice, acceptorDebit)) {
			return makeResult(false, "You do not have enough money.");
		}
		debitedMarketMoney = true;
		acceptorNewBalance = acceptorDebit.newBalance;

		if (!creditPlayerBalance(db, offer->playerId, totalPrice, ownerNewBalance)) {
			rollbackMarketDebit(player, acceptorDebit);
			return makeResult(false, "Could not credit the seller.");
		}
		if (!deliverItemsToPlayer(db, player->getGUID(), offer->itemId, amount, deliveredOnlinePlayerId)) {
			rollbackMarketDebit(player, acceptorDebit);
			return makeResult(false, "Could not deliver the item.");
		}
		deliveredItems = deliveredOnlinePlayerId != 0;
	} else {
		if (getPlainDepotItemAmount(player, depotId, offer->itemId) < amount) {
			return makeResult(false, "You do not have enough plain items in this depot.");
		}
		if (!removePlainDepotItems(player, depotId, offer->itemId, amount)) {
			return makeResult(false, "Could not remove the depot items.");
		}
		removedDepotItems = true;
		if (!IOLoginData::savePlayerDepotItems(player)) {
			addPlainItemsToDepot(player, depotId, offer->itemId, amount);
			return makeResult(false, "Could not persist depot item removal.");
		}
		if (!deliverItemsToPlayer(db, offer->playerId, offer->itemId, amount, deliveredOnlinePlayerId)) {
			addPlainItemsToDepot(player, depotId, offer->itemId, amount);
			return makeResult(false, "Could not deliver the item to the buyer.");
		}
		deliveredItems = deliveredOnlinePlayerId != 0;
		if (!creditPlayerBalance(db, player->getGUID(), totalPrice, acceptorNewBalance)) {
			if (deliveredItems) {
				if (std::shared_ptr<Player> target = g_game.getPlayerByID(deliveredOnlinePlayerId)) {
					removePlainInboxItems(target.get(), offer->itemId, amount);
				}
			}
			addPlainItemsToDepot(player, depotId, offer->itemId, amount);
			return makeResult(false, "Could not credit your bank balance.");
		}
	}

	if (!insertHistory(db, *offer, amount, MARKET_STATE_ACCEPTED) || !updateStatistics(db, *offer, amount)) {
		if (debitedMarketMoney) {
			rollbackMarketDebit(player, acceptorDebit);
		}
		if (deliveredItems) {
			if (std::shared_ptr<Player> target = g_game.getPlayerByID(deliveredOnlinePlayerId)) {
				removePlainInboxItems(target.get(), offer->itemId, amount);
			}
		}
		if (removedDepotItems) {
			addPlainItemsToDepot(player, depotId, offer->itemId, amount);
		}
		return makeResult(false, "Could not finalize the market offer.");
	}

	if (!transaction.commit()) {
		if (debitedMarketMoney) {
			rollbackMarketDebit(player, acceptorDebit);
		}
		if (deliveredItems) {
			if (std::shared_ptr<Player> target = g_game.getPlayerByID(deliveredOnlinePlayerId)) {
				removePlainInboxItems(target.get(), offer->itemId, amount);
			}
		}
		if (removedDepotItems) {
			addPlainItemsToDepot(player, depotId, offer->itemId, amount);
		}
		return makeResult(false, "Could not commit market acceptance.");
	}

	player->setBankBalance(acceptorNewBalance);
	if (offer->sale == MARKET_ACTION_SELL) {
		setOnlineBalance(offer->playerId, ownerNewBalance);
	}

	return makeResult(true, "Market offer accepted.", offer->itemId, offer->playerName);
}

uint32_t MarketManager::expireOffers(uint32_t limit)
{
	Database& db = Database::getInstance();
	const uint64_t expiredBefore = getNow() - getOfferDuration();
	DBResult_ptr result = db.storeQuery(
	    fmt::format("SELECT `id` FROM `market_offers` WHERE `created` <= {:d} ORDER BY `created` ASC LIMIT {:d}",
	                expiredBefore, limit));
	if (!result) {
		return 0;
	}

	std::vector<uint32_t> offerIds;
	do {
		offerIds.push_back(result->getNumber<uint32_t>("id"));
	} while (result->next());

	uint32_t expired = 0;
	for (uint32_t offerId : offerIds) {
		DBTransaction transaction;
		if (!transaction.begin()) {
			continue;
		}

		std::optional<MarketDbOffer> offer = getOfferForUpdate(db, offerId);
		if (!offer || offer->created + getOfferDuration() > getNow()) {
			continue;
		}

		uint64_t ownerNewBalance = 0;
		uint32_t deliveredOnlinePlayerId = 0;
		bool deliveredItems = false;

		if (!deleteOffer(db, offer->id, offer->playerId)) {
			continue;
		}

		if (offer->sale == MARKET_ACTION_BUY) {
			if (!creditPlayerBalance(db, offer->playerId, static_cast<uint64_t>(offer->price) * offer->amount,
			                         ownerNewBalance)) {
				continue;
			}
		} else {
			if (!deliverItemsToPlayer(db, offer->playerId, offer->itemId, offer->amount, deliveredOnlinePlayerId)) {
				continue;
			}
			deliveredItems = deliveredOnlinePlayerId != 0;
		}

		if (!insertHistory(db, *offer, offer->amount, MARKET_STATE_EXPIRED)) {
			if (deliveredItems) {
				if (std::shared_ptr<Player> target = g_game.getPlayerByID(deliveredOnlinePlayerId)) {
					removePlainInboxItems(target.get(), offer->itemId, offer->amount);
				}
			}
			continue;
		}

		if (!transaction.commit()) {
			if (deliveredItems) {
				if (std::shared_ptr<Player> target = g_game.getPlayerByID(deliveredOnlinePlayerId)) {
					removePlainInboxItems(target.get(), offer->itemId, offer->amount);
				}
			}
			continue;
		}

		if (offer->sale == MARKET_ACTION_BUY) {
			setOnlineBalance(offer->playerId, ownerNewBalance);
		}
		++expired;
	}

	return expired;
}
