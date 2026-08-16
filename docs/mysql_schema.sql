CREATE DATABASE IF NOT EXISTS `20250113im`
  DEFAULT CHARACTER SET utf8mb4
  COLLATE utf8mb4_unicode_ci;

USE `20250113im`;

CREATE TABLE IF NOT EXISTS `t_user` (
  `id` INT NOT NULL AUTO_INCREMENT,
  `name` VARCHAR(30) NOT NULL,
  `tel` VARCHAR(15) NOT NULL,
  `passwd` VARCHAR(64) NOT NULL,
  `salt` VARCHAR(32) NOT NULL DEFAULT '',
  `feeling` VARCHAR(100) NOT NULL DEFAULT '',
  `iconid` INT NOT NULL DEFAULT 0,
  PRIMARY KEY (`id`),
  UNIQUE KEY `uk_t_user_name` (`name`),
  UNIQUE KEY `uk_t_user_tel` (`tel`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- 密码存储说明（对齐 QQNT 密码安全）：
--   客户端先对明文密码做 SHA-256（64 字符 hex）后传输；
--   服务端生成随机盐（16 字节 → 32 字符 hex），存 passwd = SHA-256(salt + 客户端哈希)。
-- 已存在旧库（明文 passwd、无 salt）需执行：
--   ALTER TABLE t_user MODIFY passwd VARCHAR(64) NOT NULL, ADD COLUMN salt VARCHAR(32) NOT NULL DEFAULT '';
--   旧用户密码为明文，无法再登录，需重新注册。

CREATE TABLE IF NOT EXISTS `t_friend` (
  `idA` INT NOT NULL,
  `idB` INT NOT NULL,
  PRIMARY KEY (`idA`, `idB`),
  KEY `idx_t_friend_idB` (`idB`),
  CONSTRAINT `fk_t_friend_idA` FOREIGN KEY (`idA`) REFERENCES `t_user` (`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_t_friend_idB` FOREIGN KEY (`idB`) REFERENCES `t_user` (`id`) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS `offline_msg` (
  `id` INT NOT NULL AUTO_INCREMENT,
  `sender_id` INT NOT NULL,
  `receiver_id` INT NOT NULL,
  `content` TEXT NOT NULL,
  `is_delivered` TINYINT(1) NOT NULL DEFAULT 0,
  `send_time` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`),
  KEY `idx_offline_msg_receiver_delivery_time` (`receiver_id`, `is_delivered`, `send_time`),
  KEY `idx_offline_msg_sender` (`sender_id`),
  CONSTRAINT `fk_offline_msg_sender` FOREIGN KEY (`sender_id`) REFERENCES `t_user` (`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_offline_msg_receiver` FOREIGN KEY (`receiver_id`) REFERENCES `t_user` (`id`) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
