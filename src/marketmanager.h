// Copyright 2026 The Forgotten Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#ifndef FS_MARKETMANAGER_H
#define FS_MARKETMANAGER_H

#include <cstdint>
#include <string>

class Player;

struct MarketOperationResult
{
	bool success = false;
	std::string message;
	uint16_t browseId = 0;
	std::string counterpartyName;
};

class MarketManager
{
public:
	static MarketManager& getInstance()
	{
		static MarketManager instance;
		return instance;
	}

	MarketOperationResult createOffer(Player* player, uint8_t action, uint16_t itemId, uint16_t amount,
	                                  uint32_t price, bool anonymous, uint16_t depotId);
	MarketOperationResult cancelOffer(Player* player, uint32_t offerId, uint16_t depotId);
	MarketOperationResult acceptOffer(Player* player, uint32_t offerId, uint16_t amount, uint16_t depotId);

	uint32_t expireOffers(uint32_t limit = 100);

private:
	MarketManager() = default;
};

#endif
