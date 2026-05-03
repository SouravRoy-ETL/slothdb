-- q01
SELECT cab_type, COUNT(*) AS count FROM trips GROUP BY cab_type;

-- q02
SELECT passenger_count, AVG(total_amount) AS avg_total_amount
FROM trips GROUP BY passenger_count;

-- q03
SELECT passenger_count, DATE_PART('year', pickup_datetime) AS year, COUNT(*) AS count
FROM trips GROUP BY passenger_count, year;

-- q04
SELECT passenger_count, DATE_PART('year', pickup_datetime) AS year,
       ROUND(trip_distance) AS distance, COUNT(*) AS count
FROM trips
GROUP BY passenger_count, year, distance
ORDER BY year, count DESC;
