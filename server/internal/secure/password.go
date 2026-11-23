package secure

import "golang.org/x/crypto/bcrypt"

const dummyHash = "$2a$12$C6UzMDM.H6dfI/f/IKcEe.7W5dQp1Cq3jS6V0m6h4vJ0o8n2zYw3."

func HashPassword(password string) (string, error) {
	value, err := bcrypt.GenerateFromPassword([]byte(password), 12)
	if err != nil {
		return "", err
	}
	return string(value), nil
}

func CheckPassword(hash, password string) bool {
	err := bcrypt.CompareHashAndPassword([]byte(hash), []byte(password))
	return err == nil
}

func BurnPassword(password string) {
	_ = bcrypt.CompareHashAndPassword([]byte(dummyHash), []byte(password))
}
