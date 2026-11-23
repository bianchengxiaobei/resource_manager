package api

import "testing"

func TestPercentageBucketMatchesCpp(t *testing.T) {
	if value := PercentageBucket("player", "game", "salt"); value != 9195 {
		t.Fatalf("bucket = %d, want 9195", value)
	}
	if CompareVersions("1.2.0", "1.10.0") >= 0 {
		t.Fatal("1.2.0 must sort before 1.10.0")
	}
	if CompareVersions("v1.0.0", "1.0") != 0 {
		t.Fatal("equivalent versions should compare equal")
	}
}
