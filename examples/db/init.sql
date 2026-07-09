CREATE TABLE IF NOT EXISTS users (
    id       VARCHAR(64) PRIMARY KEY,
    password VARCHAR(256) NOT NULL,
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS sessions (
    token    VARCHAR(128) PRIMARY KEY,
    user_id  VARCHAR(64) NOT NULL,
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (user_id) REFERENCES users(id)
);

CREATE TABLE IF NOT EXISTS chat_messages (
    id         BIGINT AUTO_INCREMENT PRIMARY KEY,
    session_id VARCHAR(64) NOT NULL,
    role       VARCHAR(16) NOT NULL,
    content    TEXT NOT NULL,
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_session (session_id, created_at)
);

INSERT IGNORE INTO users (id, password) VALUES ('test', 'test123');
INSERT IGNORE INTO users (id, password) VALUES ('admin', 'admin123');
