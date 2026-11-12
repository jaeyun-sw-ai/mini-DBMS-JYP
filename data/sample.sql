-- 예시: ./minidb data/sample.sql
INSERT INTO users (name, age) VALUES ('kim', 25);
INSERT INTO users VALUES ('lee', 31), ('park', 28);
SELECT * FROM users;
SELECT name, age FROM users WHERE id = 2;
SELECT * FROM users WHERE age >= 28;
