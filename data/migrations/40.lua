function onUpdateDatabase()
	logMigration("> Updating database to version 41 (market transaction statistics)")
	db.query([[
		CREATE TABLE IF NOT EXISTS `market_statistics` (
			`itemtype` SMALLINT UNSIGNED NOT NULL,
			`sale` TINYINT(1) NOT NULL DEFAULT 0,
			`day` INT UNSIGNED NOT NULL,
			`transactions` INT UNSIGNED NOT NULL DEFAULT 0,
			`total_price` BIGINT UNSIGNED NOT NULL DEFAULT 0,
			`highest_price` INT UNSIGNED NOT NULL DEFAULT 0,
			`lowest_price` INT UNSIGNED NOT NULL DEFAULT 0,
			PRIMARY KEY (`itemtype`, `sale`, `day`)
		) ENGINE=InnoDB DEFAULT CHARACTER SET=utf8
	]])
	return true
end
