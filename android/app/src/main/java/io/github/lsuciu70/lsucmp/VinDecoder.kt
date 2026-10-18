// What can be read from a VIN on the phone, without any online service:
// manufacturer and country (ISO 3779 WMI, positions 1-3), model year
// (position 10), and for Volkswagen cars the model and plant codes
// (positions 7-8 and 11). Only codes known with confidence are listed;
// anything else is left out rather than guessed. The VIN carries no color,
// trim or options: those exist only in the manufacturer's systems.
package io.github.lsuciu70.lsucmp

import java.time.Year

object VinDecoder {

    /** Decoded fields in display order (label to value); empty if nothing. */
    fun decode(vin: String, currentYear: Int = Year.now().value): List<Pair<String, String>> {
        val v = vin.trim().uppercase()
        if (v.length != 17 || v.any { it == 'I' || it == 'O' || it == 'Q' || !it.isLetterOrDigit() })
            return emptyList()
        val out = mutableListOf<Pair<String, String>>()
        val wmi = v.substring(0, 3)
        MANUFACTURERS[wmi]?.let { out += "Manufacturer" to it }
        country(v)?.let { out += "Country" to it }
        if (wmi in VW_CARS) {
            VW_MODELS[v.substring(6, 8)]?.let { out += "Model" to it }
        }
        modelYear(v[9], currentYear)?.let { out += "Model year" to it.toString() }
        if (wmi in VW_CARS) {
            VW_PLANTS[v[10]]?.let { out += "Plant" to it }
        }
        if (out.isNotEmpty()) out += "Serial number" to v.substring(11)
        return out
    }

    /**
     * Position 10 repeats every 30 years (A = 1980 or 2010, ...); take the
     * latest year not after next year.
     */
    fun modelYear(code: Char, currentYear: Int): Int? {
        val i = YEAR_CODES.indexOf(code)
        if (i < 0) return null
        var year = 1980 + i
        while (year + 30 <= currentYear + 1) year += 30
        return year
    }

    private const val YEAR_CODES = "ABCDEFGHJKLMNPRSTVWXY123456789"

    private fun country(v: String): String? {
        val two = v.substring(0, 2)
        return when {
            v[0] == 'W' -> "Germany"
            two in listOf("VF", "VR") -> "France"
            two == "VS" -> "Spain"
            two == "TM" -> "Czech Republic"
            two == "TR" -> "Hungary"
            v[0] == 'Z' -> "Italy"
            v[0] == 'S' && v[1] in 'A'..'M' -> "United Kingdom"
            v[0] == 'Y' && v[1] in 'S'..'W' -> "Sweden"
            v[0] == 'J' -> "Japan"
            v[0] == 'K' && v[1] in 'L'..'R' -> "South Korea"
            v[0] == 'L' -> "China"
            v[0] in "145" -> "United States"
            v[0] == '2' -> "Canada"
            v[0] == '3' -> "Mexico"
            else -> null
        }
    }

    private val MANUFACTURERS = mapOf(
        "WVW" to "Volkswagen (passenger car)",
        "WVG" to "Volkswagen (SUV / off-road)",
        "WV1" to "Volkswagen Commercial Vehicles",
        "WV2" to "Volkswagen Commercial Vehicles (bus / van)",
        "WAU" to "Audi",
        "WUA" to "Audi Sport (quattro GmbH)",
        "WP0" to "Porsche (passenger car)",
        "WP1" to "Porsche (SUV)",
        "TMB" to "Škoda",
        "VSS" to "SEAT",
        "WBA" to "BMW",
        "WBS" to "BMW M",
        "WDD" to "Mercedes-Benz (passenger car)",
        "WME" to "smart",
        "WF0" to "Ford Germany",
        "W0L" to "Opel",
    )

    private val VW_CARS = setOf("WVW", "WVG")

    // VW model codes (positions 7-8), only the well-known ones.
    private val VW_MODELS = mapOf(
        "1K" to "Golf V / Jetta",
        "5K" to "Golf VI",
        "AU" to "Golf VII (MQB)",
        "CD" to "Golf VIII",
        "6R" to "Polo V",
        "AW" to "Polo VI",
        "3G" to "Passat B8",
        "5N" to "Tiguan I",
        "AD" to "Tiguan II",
    )

    // VW plant codes (position 11), only the well-known ones.
    private val VW_PLANTS = mapOf(
        'W' to "Wolfsburg",
        'P' to "Mosel / Zwickau",
        'E' to "Emden",
        'K' to "Osnabrück",
        'M' to "Puebla (Mexico)",
    )
}
