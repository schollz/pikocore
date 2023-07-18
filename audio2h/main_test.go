package main

import (
	"testing"

	"github.com/stretchr/testify/assert"
)

func TestGuessBPM(t *testing.T) {

	type test struct {
		fname string
		beats float64
		bpm   float64
	}

	tests := []test{
		{"flacs/cold_sweat_bpm150_beats16.flac", 16, 150},
		{"flacs/amen_5c063f57_beats8_bpm146.flac", 8, 146},
		{"test.flac", 4, 160},
	}
	for _, tc := range tests {
		beats, bpm, err := determineBeats(tc.fname)
		assert.Nil(t, err)
		assert.Equal(t, tc.bpm, bpm)
		assert.Equal(t, tc.beats, beats)
	}
}
